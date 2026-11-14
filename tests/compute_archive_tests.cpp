#include "app/compute_archive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <zstd.h>

#include <iostream>

namespace {

// A tar with one file of `size` bytes, as compute_modules.py writes them: header, data padded to 512, two
// zero blocks, the whole padded to 10240.
QByteArray Tar(const QByteArray &name, const QByteArray &data) {
    QByteArray header(512, '\0');
    header.replace(0, name.size(), name);
    header.replace(100, 7, "0000644");
    header.replace(108, 7, "0000000");
    header.replace(116, 7, "0000000");
    header.replace(124, 11, QByteArray::number(data.size(), 8).rightJustified(11, '0'));
    header.replace(136, 11, "00000000000");
    header[156] = '0';
    header.replace(257, 5, "ustar");
    header.replace(148, 8, "        ");
    unsigned sum = 0;
    for (const char c : header) sum += static_cast<unsigned char>(c);
    header.replace(148, 7, QByteArray::number(sum, 8).rightJustified(6, '0') + '\0');
    QByteArray tar = header + data + QByteArray((512 - data.size() % 512) % 512, '\0') + QByteArray(1024, '\0');
    tar.append(QByteArray((10240 - tar.size() % 10240) % 10240, '\0'));
    return tar;
}

bool RoundTrip(qsizetype tarSize, const QTemporaryDir &root) {
    // the file's size that gives a tar of exactly tarSize bytes
    const qsizetype fileSize = tarSize - 512 - 1024;
    QByteArray data(fileSize, '\0');
    for (qsizetype i = 0; i < fileSize; ++i) data[i] = static_cast<char>((i * 2654435761u) >> 13);
    const QByteArray tar = Tar("module.bin", data);
    if (tar.size() != tarSize) {
        std::cerr << "test tar is " << tar.size() << " bytes, wanted " << tarSize << "\n";
        return false;
    }
    QByteArray packed(static_cast<qsizetype>(ZSTD_compressBound(tar.size())), '\0');
    const std::size_t packedSize = ZSTD_compress(packed.data(), packed.size(), tar.constData(), tar.size(), 3);
    packed.truncate(static_cast<qsizetype>(packedSize));
    const QString archive = root.filePath(QStringLiteral("%1.tar.zst").arg(tarSize));
    QFile file(archive);
    if (!file.open(QIODevice::WriteOnly) || file.write(packed) != packed.size()) return false;
    file.close();
    const QString folder = root.filePath(QStringLiteral("out-%1").arg(tarSize));
    QString error;
    if (!forevertas::app::ExtractComputeArchive(archive, folder, &error)) {
        std::cerr << "tar of " << tarSize << " bytes: " << error.toStdString() << "\n";
        return false;
    }
    QFile unpacked(QDir(folder).filePath(QStringLiteral("module.bin")));
    if (!unpacked.open(QIODevice::ReadOnly) || unpacked.readAll() != data) {
        std::cerr << "tar of " << tarSize << " bytes: wrong contents\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    const auto block = static_cast<qsizetype>(ZSTD_DStreamOutSize());
    bool okay = true;
    // decompressed sizes that end exactly at the end of zstd's output buffer (the sm90 module of
    // 2026-11-14 was 110 of them; a tar is whole 10240-byte records), and ones that do not
    for (const qsizetype tarSize : {5 * block, 110 * block, 5 * block + 10240, qsizetype{20480}})
        okay = RoundTrip(tarSize, root) && okay;
    std::cout << (okay ? "compute archive tests passed\n" : "compute archive tests FAILED\n");
    return okay ? 0 : 1;
}
