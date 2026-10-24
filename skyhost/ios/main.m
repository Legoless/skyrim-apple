#import "frontend.h"
#import "../profile.h"
#import "../runtime.h"
#import "../skyhost.h"
#import <Foundation/Foundation.h>

static NSString *g_rewrite_directory;
static NSString *g_setup_error;

static int run_bundled_guest(void) {
    @autoreleasepool {
        LOG("PROFILE name=%s title_id=%s modules=%s,%s platform=ios", sky_profile.name,
            sky_profile.title_id, sky_profile.modules[0], sky_profile.modules[1]);
        load_modules(g_rewrite_directory.fileSystemRepresentation, sky_profile.modules, (int)sky_profile.module_count);
        return runtime_run();
    }
}
static NSString *configured_path(const char *variable, NSString *fallback) {
    const char *value = getenv(variable);
    return value && *value ? [NSString stringWithUTF8String:value] : fallback;
}
static BOOL is_directory(NSString *path) {
    BOOL directory = NO;
    return path && [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] && directory;
}

/* Repeated on foreground/Retry after the user copies files through Files or
 * Finder. This prepares paths only; loading the guest stays on its thread. */
const char *sky_ios_check_game_files(void) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
    NSString *skyrim = [documents stringByAppendingPathComponent:@"Skyrim"];
    NSString *save = configured_path("SKY_SAVE_DIR", [documents stringByAppendingPathComponent:@"save"]);
    NSString *romfs = configured_path("SKY_ROMFS", [skyrim stringByAppendingPathComponent:@"RomFS"]);
    NSString *shaders = configured_path("SKY_SHADER_CACHE_DIR", [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"shaders"]);
    g_rewrite_directory = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"rewrite"];
    NSError *error = nil;
    g_setup_error = nil;
    if (![files createDirectoryAtPath:skyrim withIntermediateDirectories:YES attributes:nil error:&error] ||
        ![files createDirectoryAtPath:save withIntermediateDirectories:YES attributes:nil error:&error]) {
        g_setup_error = [@"Unable to prepare the game's save folder.\n\n" stringByAppendingString:error.localizedDescription];
    } else {
        setenv("SKY_SAVE_DIR", save.fileSystemRepresentation, 1);
        setenv("SKY_ROMFS", romfs.fileSystemRepresentation, 1);
        setenv("SKY_SHADER_CACHE_DIR", shaders.fileSystemRepresentation, 1);
        if (!is_directory(romfs) || !is_directory([romfs stringByAppendingPathComponent:@"Data"]) ||
            ![files isReadableFileAtPath:[romfs stringByAppendingPathComponent:@"Skyrim.ini"]]) {
            g_setup_error = @"Add your Skyrim files\n\nCopy the extracted RomFS folder to Skyrim/RomFS in this app's Documents using Files or Finder, then return here or tap Retry.\n\nYour save files are kept separately in save.";
        } else if (!is_directory(shaders) ||
            ![files isReadableFileAtPath:[g_rewrite_directory stringByAppendingPathComponent:@"main.fixups.tsv"]] ||
            ![files isReadableFileAtPath:[g_rewrite_directory stringByAppendingPathComponent:@"main.gates.tsv"]] ||
            ![files isReadableFileAtPath:[g_rewrite_directory stringByAppendingPathComponent:@"sdk.fixups.tsv"]] ||
            ![files isReadableFileAtPath:[g_rewrite_directory stringByAppendingPathComponent:@"sdk.gates.tsv"]]) {
            g_setup_error = @"The app's game support files are missing.\n\nBuild and install the app again with the rewritten modules and translated shader cache.";
        }
    }
    return g_setup_error.UTF8String;
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        /* UI-automation launches have no console; SKY_LOG_FILE names a file in
         * Documents that receives the [skyhost] log instead. */
        NSString *logName = NSProcessInfo.processInfo.environment[@"SKY_LOG_FILE"];
        if (logName.length && ![logName containsString:@"/"]) {
            NSString *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                                       inDomains:NSUserDomainMask].firstObject.path;
            NSString *logPath = [documents stringByAppendingPathComponent:logName];
            if (freopen(logPath.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOFBF, 1 << 20);
        }
        sky_ios_set_setup_check(sky_ios_check_game_files);
        BOOL controlsTest = [NSProcessInfo.processInfo.arguments containsObject:@"--controls-test-mode"] ||
            [NSProcessInfo.processInfo.environment[@"SKY_CONTROLS_TEST_MODE"] isEqualToString:@"1"];
        if (controlsTest) return sky_ios_run(NULL);
        sky_ios_set_setup_message(sky_ios_check_game_files());
        return sky_ios_run(run_bundled_guest);
    }
}
