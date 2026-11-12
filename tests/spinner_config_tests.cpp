#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <impl/config/config_store.hxx>
#include <cstdio>
#include <sstream>
static int checks=0,failures=0;
static void test(const char* text,float lo,float hi) {
    config::settings_t s;std::istringstream in(text);config::read_profile(in,s);++checks;
    if(std::abs(s.autobot_spinner_rpm_min-lo)>.001f || std::abs(s.autobot_spinner_rpm_max-hi)>.001f) {
        ++failures;std::printf("FAIL migration %s -> %.2f..%.2f\n",text,s.autobot_spinner_rpm_min,s.autobot_spinner_rpm_max);
    }
}
int main() {
    test("",400,440);test("autobot.spinner_rpm=360",342,378);test("autobot.spinner_rpm=420",400,440);
    test("autobot.spinner_rpm=477",457,477);test("autobot.spinner_rpm=0",0,0);
    test("autobot.spinner_rpm=nan",400,440);
    test("autobot.spinner_rpm_min=330\nautobot.spinner_rpm_max=390",330,390);
    test("autobot.spinner_rpm=420\nautobot.spinner_rpm_min=330\nautobot.spinner_rpm_max=390",330,390);
    test("autobot.spinner_rpm_min=330\nautobot.spinner_rpm_max=390\nautobot.spinner_rpm=420",330,390);
    test("autobot.spinner_rpm_min=390\nautobot.spinner_rpm_max=330",330,390);
    test("autobot.spinner_rpm_min=380",380,380);test("autobot.spinner_rpm_max=350",350,350);
    test("autobot.spinner_rpm_min=-10\nautobot.spinner_rpm_max=900",0,477);
    std::printf("Spinner config migration: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
