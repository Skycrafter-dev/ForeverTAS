// "Go to" on a made-up race of 60 s with checkpoints at 10.23 s and 25.5 s
// and the finish at 41.07 s: every way of writing a place lands on its tick.
#include "viewer/go_to.h"

#include <iostream>
#include <string>

namespace {

using namespace forevertas::viewer;

int failures = 0;

void Expect(const char *typed, qint64 currentMs, std::optional<qint64> wanted) {
    GoToContext context;
    context.currentMs = currentMs;
    context.durationMs = 60000;
    context.marks = {{10230, 1, false}, {25500, 2, false}, {41070, 3, true}};
    const GoToResult result = ResolveGoTo(QString::fromUtf8(typed), context);
    if (result.timeMs != wanted || (!wanted && result.error.isEmpty())) {
        std::cerr << "\"" << typed << "\" from " << currentMs << " ms: got "
                  << (result.timeMs ? std::to_string(*result.timeMs) : "no time (" + result.error.toStdString() + ")")
                  << ", wanted " << (wanted ? std::to_string(*wanted) : std::string("no time")) << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    Expect("30", 0, 30000);
    Expect("30.5", 0, 30500);
    Expect("30,5", 0, 30500);
    Expect("1:02.35", 0, 60000);  // past the end: the end
    Expect("0:12.34", 0, 12340);
    Expect("0:00:41.07", 0, 41070);
    Expect("1500ms", 0, 1500);
    Expect("1500 ms", 0, 1500);
    Expect("2.5s", 0, 2500);
    Expect("0m 30s", 0, 30000);
    Expect("12.345", 0, 12350);  // the nearest tick
    Expect("+2", 10000, 12000);
    Expect("-0.5", 10000, 9500);
    Expect("- 500ms", 10000, 9500);
    Expect("-20", 10000, 0);
    Expect("cp 2", 0, 25500);
    Expect("CP1", 0, 10230);
    Expect("checkpoint 2", 0, 25500);
    Expect("cp 3", 0, std::nullopt);  // the third mark is the finish
    Expect("finish", 0, 41070);
    Expect("start", 30000, 0);
    Expect("end", 0, 60000);
    Expect("next", 10230, 25500);
    Expect("next cp", 0, 10230);
    Expect("prev", 25500, 10230);
    Expect("previous", 5000, std::nullopt);
    Expect("next", 41070, std::nullopt);
    Expect("50%", 0, 30000);
    Expect("tick 120", 0, 1200);
    Expect("t5", 0, 50);
    Expect("", 0, std::nullopt);
    Expect("banana", 0, std::nullopt);
    Expect("30 5", 0, std::nullopt);
    Expect("1:75", 0, std::nullopt);
    if (failures != 0) return 1;
    std::cout << "every way of writing a place lands on its tick\n";
    return 0;
}
