#include "app/compute_archive.h"

#include <QDir>
#include <QFile>

#include <zstd.h>

#include <memory>
#include <vector>

namespace forevertas::app {
namespace {

// A tar stream read block by block: 512-byte headers, data padded to 512.
class TarWriter {
public:
    explicit TarWriter(const QString &directory) : directory_(directory) {}

    bool Feed(const char *data, std::size_t size, QString *error) {
        while (size > 0) {
            if (remaining_ == 0 && padding_ == 0) {
                const std::size_t take = std::min(size, std::size_t{512} - static_cast<std::size_t>(header_.size()));
                header_.append(data, static_cast<qsizetype>(take));
                data += take;
                size -= take;
                if (header_.size() == 512 && !StartEntry(error)) return false;
                continue;
            }
            if (remaining_ > 0) {
                const std::size_t take = static_cast<std::size_t>(
                        std::min<quint64>(size, remaining_));
                if (file_ && file_->write(data, static_cast<qint64>(take)) != static_cast<qint64>(take)) {
                    *error = QStringLiteral("cannot write %1").arg(file_->fileName());
                    return false;
                }
                data += take;
                size -= take;
                remaining_ -= take;
                if (remaining_ == 0 && file_) {
                    file_->close();
                    file_.reset();
                }
                continue;
            }
            const std::size_t take = static_cast<std::size_t>(std::min<quint64>(size, padding_));
            data += take;
            size -= take;
            padding_ -= take;
        }
        return true;
    }

    bool Finished() const { return ended_ && remaining_ == 0; }

private:
    bool StartEntry(QString *error) {
        const QByteArray header = std::exchange(header_, {});
        if (header.count('\0') == 512) {   // the end
            ended_ = true;
            return true;
        }
        const QByteArray name = header.left(100).split('\0').front();
        bool parsed = false;
        const quint64 size = header.mid(124, 12).split('\0').front().trimmed().toULongLong(&parsed, 8);
        const char type = header.at(156);
        if (!parsed || ended_) {
            *error = QStringLiteral("damaged archive");
            return false;
        }
        remaining_ = size;
        padding_ = (512 - size % 512) % 512;
        if (type != '0' && type != '\0') return true;   // only plain files
        const QString fileName = QString::fromUtf8(name);
        if (fileName.isEmpty() || fileName.contains(QLatin1Char('/')) ||
            fileName.contains(QLatin1Char('\\')) || fileName.startsWith(QLatin1Char('.'))) {
            *error = QStringLiteral("unexpected file %1 in the archive").arg(fileName);
            return false;
        }
        file_ = std::make_unique<QFile>(QDir(directory_).filePath(fileName));
        if (!file_->open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("cannot write %1").arg(file_->fileName());
            return false;
        }
        if (size == 0) file_.reset();
        return true;
    }

    QString directory_;
    QByteArray header_;
    quint64 remaining_ = 0;
    quint64 padding_ = 0;
    bool ended_ = false;
    std::unique_ptr<QFile> file_;
};

}  // namespace

bool ExtractComputeArchive(const QString &archive, const QString &directory, QString *error) {
    QFile input(archive);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1").arg(archive);
        return false;
    }
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    const std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> stream(ZSTD_createDStream(),
                                                                             &ZSTD_freeDStream);
    // archives are compressed with --long=27: allow its window
    ZSTD_DCtx_setParameter(stream.get(), ZSTD_d_windowLogMax, 27);
    TarWriter tar(directory);
    std::vector<char> in(ZSTD_DStreamInSize());
    std::vector<char> out(ZSTD_DStreamOutSize());
    std::size_t last = 0;
    while (true) {
        const qint64 read = input.read(in.data(), static_cast<qint64>(in.size()));
        if (read < 0) {
            *error = QStringLiteral("cannot read %1").arg(archive);
            return false;
        }
        if (read == 0) break;
        ZSTD_inBuffer source{in.data(), static_cast<std::size_t>(read), 0};
        bool more = true;
        while (more) {   // also while the output fills up: zstd still holds data
            ZSTD_outBuffer target{out.data(), out.size(), 0};
            last = ZSTD_decompressStream(stream.get(), &target, &source);
            if (ZSTD_isError(last)) {
                *error = QStringLiteral("damaged archive (%1)").arg(QString::fromUtf8(ZSTD_getErrorName(last)));
                return false;
            }
            if (!tar.Feed(out.data(), target.pos, error)) return false;
            // a frame that ends exactly at the end of the output is done (0):
            // asking again would start the next frame and lose that 0
            more = source.pos < source.size || (target.pos == target.size && last != 0);
        }
    }
    if (last != 0 || !tar.Finished()) {
        *error = QStringLiteral("the archive is incomplete");
        return false;
    }
    return true;
}

}  // namespace forevertas::app
