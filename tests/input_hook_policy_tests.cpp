#include <impl/input/hook_policy.hxx>
#include <cstdio>
#include <initializer_list>
int main() {
    int failures=0, checks=0;
    for(bool enabled:{false,true}) for(bool smart:{false,true}) for(bool tap:{false,true}) {
        const bool expected=tap||(enabled&&smart);
        ++checks;
        if(input::needs_keyboard_hook(enabled,smart,tap)!=expected) ++failures;
    }
    std::printf("Input hook policy: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
