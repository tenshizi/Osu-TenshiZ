#define NOMINMAX
#include <impl/memory/process.hxx>
#include <impl/defs/offsets_lazer.hxx>
#include <cstdio>
int main() {
    memory::c_process p;p.enable_fallback();
    if(!p.attach(memory::c_process::find_pid_by_name(L"osu!.exe"))) return 1;
    uint64_t root=0xbf3e3cc0;
    offsets::lazer::table_t o;
    for(int i=0;i<3;++i) {
        const auto api=p.read<uint64_t>(root+o.game_base_api);
        const auto back=p.read<uint64_t>(api+o.api_access_game);
        const auto screens=p.read<uint64_t>(root+o.game_screen_stack);
        const auto stack=p.read<uint64_t>(screens+o.screen_stack_stack);
        const auto array=p.read<uint64_t>(stack+8);
        const auto count=p.read<int>(stack+0x10);
        std::printf("root=%llx mt=%llx api=%llx back=%llx screens=%llx stack=%llx array=%llx count=%d\n",root,p.read<uint64_t>(root),api,back,screens,stack,array,count);
        if(!back||back==root) break;
        root=back;
    }
}
