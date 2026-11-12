#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <impl/ui/elements.hxx>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
static void check( bool ok, const char* what ) {
    ++checks;
    if ( !ok ) { ++failures; std::printf( "FAIL: %s\n", what ); }
}
static void frame( char* buffer, size_t size, bool focus = false ) {
    ImGui::NewFrame( );
    ImGui::SetNextWindowPos( ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( 400, 200 ) );
    ImGui::Begin( "test", nullptr, ImGuiWindowFlags_NoSavedSettings );
    if ( focus ) ImGui::SetKeyboardFocusHere( );
    ui::text_input( "##name", buffer, static_cast<int>( size ), 250 );
    ImGui::End( );
    ImGui::Render( );
}
int main( ) {
    ImGui::CreateContext( );
    auto& io = ImGui::GetIO( );
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2( 800, 600 );
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32( &pixels, &width, &height );
    char name[64]{};
    frame( name, sizeof name, true );
    frame( name, sizeof name );
    check( ui::_ti::focused_id != 0, "field advertises text focus to overlay" );
    check( ImGui::GetCurrentContext( )->PlatformImeData.WantTextInput,
        "field requests keyboard character input in the current frame" );
    io.AddInputCharactersUTF8( "SliderPractice" );
    frame( name, sizeof name );
    check( std::strcmp( name, "SliderPractice" ) == 0, "typed profile name reaches buffer" );
    io.AddKeyEvent( ImGuiKey_Backspace, true );
    frame( name, sizeof name );
    io.AddKeyEvent( ImGuiKey_Backspace, false );
    frame( name, sizeof name );
    check( std::strcmp( name, "SliderPractic" ) == 0, "Backspace edits name" );
    io.AddKeyEvent( ImGuiMod_Ctrl, true );
    io.AddKeyEvent( ImGuiKey_A, true );
    frame( name, sizeof name );
    io.AddKeyEvent( ImGuiKey_A, false );
    io.AddKeyEvent( ImGuiMod_Ctrl, false );
    frame( name, sizeof name );
    io.AddInputCharactersUTF8( "new_config" );
    frame( name, sizeof name );
    check( std::strcmp( name, "new_config" ) == 0, "select-all replaces name" );
    io.AddKeyEvent( ImGuiKey_Enter, true );
    frame( name, sizeof name );
    io.AddKeyEvent( ImGuiKey_Enter, false );
    frame( name, sizeof name );
    check( ui::_ti::focused_id == 0, "Enter finishes editing" );
    check( std::strcmp( name, "new_config" ) == 0, "finishing preserves profile name" );
    name[ 0 ] = '\0';
    frame( name, sizeof name, true );
    frame( name, sizeof name );
    ImGui::GetPlatformIO( ).Platform_GetClipboardTextFn = []( ImGuiContext* ) { return "paste_config"; };
    io.AddKeyEvent( ImGuiMod_Ctrl, true );
    frame( name, sizeof name );
    io.AddKeyEvent( ImGuiKey_V, true );
    frame( name, sizeof name );
    io.AddKeyEvent( ImGuiKey_V, false );
    io.AddKeyEvent( ImGuiMod_Ctrl, false );
    frame( name, sizeof name );
    check( std::strcmp( name, "paste_config" ) == 0, "clipboard paste inserts a profile name" );
    ui::close_transient_ui( );
    check( ui::_ti::focused_id == 0 && ImGui::GetActiveID( ) == 0, "menu close clears native text editing state" );
    ImGui::DestroyContext( );
    std::printf( "Profile text input: %d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
