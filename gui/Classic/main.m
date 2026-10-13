// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
//
// RTL8723BE Wireless Utility: launcher + "classic" AppKit interface.
//
// This is the app's main executable, built for macOS 10.13 and later. SwiftUI (10.15+) and the
// Observation-based interface (macOS 14+) cannot exist on a High Sierra system, so the .app carries
// two interfaces on top of the same driver:
//
//   * macOS 14 Sonoma and later: main() hands over (exec) to RTL8723BEWirelessUtilityModern, the
//     SwiftUI app in the same bundle.
//   * macOS 10.13 - 13 (or RTL_FORCE_CLASSIC=1 / --classic anywhere): this file's plain AppKit UI,
//     which uses nothing newer than macOS 10.13 and no Swift runtime.
//
// Everything before the exec is plain C on purpose: AppKit must not be initialised first.

#import <Cocoa/Cocoa.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include "../RTL8723BEBridge.h"

#define RTL_KEXT_ID @"com.rtl8723be.macos.wifi"
#define RTL_DONATE_URL @"https://www.paypal.com/donate/?business=juanesgtgt2%40gmail.com&no_recurring=0&item_name=Issen+Software+Group"
#define RTL_WEBSITE_URL @"https://issen.kurokamicorp.com/"
#define RTL_RELEASES_URL @"https://github.com/AA-EION/RTL8723BE-macOS-WiFi/releases"

// MARK: - Launcher

/// Darwin major version: 17 = macOS 10.13, 19 = 10.15, 20 = 11, 23 = 14 (Sonoma), 25 = 26.
/// NSProcessInfo cannot be trusted here: apps linked against an old SDK are told "10.16" on Big Sur+.
static int darwinMajor(void) {
    struct utsname u;
    if (uname(&u) != 0) return 0;
    return atoi(u.release);
}

static void execModernIfSupported(char **argv) {
    if (darwinMajor() < 23) return;                       // modern UI needs macOS 14
    if (getenv("RTL_FORCE_CLASSIC")) return;
    for (char **a = argv + 1; *a; a++) if (strcmp(*a, "--classic") == 0) return;
    char self[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, resolved)) return;
    char *slash = strrchr(resolved, '/');
    if (!slash) return;
    *slash = 0;
    char modern[PATH_MAX];
    snprintf(modern, sizeof(modern), "%s/RTL8723BEWirelessUtilityModern", resolved);
    if (access(modern, X_OK) != 0) return;
    argv[0] = modern;
    execv(modern, argv);                                    // only returns on failure: fall back to classic
}

// MARK: - Model

typedef NS_ENUM(NSInteger, RTLPhase) { RTLPhaseNoDriver, RTLPhaseDisconnected, RTLPhaseScanning, RTLPhaseConnecting, RTLPhaseConnected };

@interface RTLNetwork : NSObject
@property (copy) NSString *ssid, *bssid;
@property (copy) NSData *bssidData;
@property int channel, rssi;
@property BOOL secured;
@end
@implementation RTLNetwork
- (NSString *)displayName { return self.ssid.length ? self.ssid : @"Hidden network"; }
@end

static NSString *const RTLStateChanged = @"RTLStateChanged";

static NSString *formatMAC(const uint8_t *m) {
    return [NSString stringWithFormat:@"%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]];
}

static NSString *formatBitrate(double bps) {
    if (bps >= 1e9) return [NSString stringWithFormat:@"%.1f Gbps", bps / 1e9];
    if (bps >= 1e6) return [NSString stringWithFormat:@"%.1f Mbps", bps / 1e6];
    if (bps >= 1e3) return [NSString stringWithFormat:@"%.1f Kbps", bps / 1e3];
    return [NSString stringWithFormat:@"%.0f bps", MAX(0, bps)];
}

static NSString *formatDuration(NSTimeInterval t) {
    long s = (long)t;
    if (s >= 3600) return [NSString stringWithFormat:@"%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60];
    return [NSString stringWithFormat:@"%ld:%02ld", s / 60, s % 60];
}

@interface RTLController : NSObject
@property (nonatomic) BOOL hardwarePresent, bootArgPresent, driverLoaded, isScanning, isBusy;
@property (nonatomic, copy) NSString *driverVersion, *macAddress, *ssid, *bssid, *interfaceName, *ipAddress, *lastAlert;
@property (nonatomic) RTLPhase phase;
@property (nonatomic) int channel, rssi, antenna;
@property (nonatomic, strong) NSDate *connectedSince, *lastScan;
@property (nonatomic) double rxBps, txBps;
@property (nonatomic, strong) NSArray<RTLNetwork *> *networks;
@property (nonatomic) BOOL haveDiag;
@property (nonatomic) struct RTL8723BEDiag diag;
@property (nonatomic, strong) NSMutableArray<NSString *> *logLines;
- (void)log:(NSString *)message;
- (void)refresh;
- (void)scan;
- (void)joinNetwork:(RTLNetwork *)net password:(NSString *)password;
- (void)disconnect;
- (void)setAntenna:(int)value;
- (void)loadDriver;
@end

@implementation RTLController {
    NSTimer *_timer;
    uint32_t _lastState;
    BOOL _haveCounters;
    uint64_t _lastRx, _lastTx;
    NSDate *_lastCounterTime;
    NSDateFormatter *_stamp;
}

- (instancetype)init {
    if ((self = [super init])) {
        _logLines = [NSMutableArray array];
        _networks = @[];
        _phase = RTLPhaseNoDriver;
        _antenna = 2;
        _lastState = UINT32_MAX;
        _macAddress = _ssid = _bssid = _interfaceName = _ipAddress = @"";
        _stamp = [[NSDateFormatter alloc] init];
        _stamp.dateFormat = @"HH:mm:ss";
        NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
        struct utsname u;
        uname(&u);
        [self log:[NSString stringWithFormat:@"RTL8723BE Wi-Fi %@ (classic interface) started on macOS %ld.%ld, Darwin %s",
                   [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"dev",
                   (long)v.majorVersion, (long)v.minorVersion, u.release]];
        [self refreshEnvironment];
        [self refresh];
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
    }
    return self;
}

- (void)notify { [[NSNotificationCenter defaultCenter] postNotificationName:RTLStateChanged object:self]; }

- (void)log:(NSString *)message {
    [self.logLines addObject:[NSString stringWithFormat:@"%@  %@", [_stamp stringFromDate:[NSDate date]], message]];
    if (self.logLines.count > 500) [self.logLines removeObjectsInRange:NSMakeRange(0, self.logLines.count - 500)];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"RTLLogChanged" object:self];
}

- (void)refreshEnvironment {
    self.hardwarePresent = rtl_hardware_present() == 1;
    size_t size = 0;
    sysctlbyname("kern.bootargs", NULL, &size, NULL, 0);
    char *buf = calloc(size + 1, 1);
    sysctlbyname("kern.bootargs", buf, &size, NULL, 0);
    NSString *args = [NSString stringWithUTF8String:buf] ?: @"";
    free(buf);
    self.bootArgPresent = [[args componentsSeparatedByString:@" "] containsObject:@"rtl8723be_experimental=1"];
}

- (void)refresh {
    io_connect_t conn = 0;
    if (rtl_open_client(&conn) != 0) {
        if (self.driverLoaded) [self log:@"Driver is no longer loaded."];
        self.driverLoaded = NO;
        self.phase = RTLPhaseNoDriver;
        self.interfaceName = self.ipAddress = @"";
        self.connectedSince = nil;
        self.haveDiag = NO;
        [self notify];
        return;
    }
    if (!self.driverLoaded) {
        self.driverLoaded = YES;
        [self log:@"Connected to RTL8723BEWiFi.kext."];
        [self readDriverVersion];
    }
    struct RTL8723BEStatus st;
    memset(&st, 0, sizeof(st));
    if (rtl_get_status(conn, &st) == 0) {
        self.macAddress = formatMAC(st.mac);
        self.bssid = formatMAC(st.bssid);
        st.ssid[RTL8723BE_MAX_SSID_LEN] = 0;
        self.ssid = [NSString stringWithUTF8String:st.ssid] ?: @"";
        self.channel = st.channel;
        self.rssi = st.rssi;
        self.antenna = st.antenna == 2 ? 2 : 1;
        if (st.state != _lastState) {
            if (st.state == kRTLStateConnected) {
                self.connectedSince = [NSDate date];
                [self log:[NSString stringWithFormat:@"Connected to %@ (channel %d).", self.ssid, self.channel]];
            } else if (_lastState == kRTLStateConnected) {
                self.connectedSince = nil;
                [self log:@"Disconnected from the network."];
            }
            _lastState = st.state;
        }
        switch (st.state) {
            case kRTLStateConnected: self.phase = RTLPhaseConnected; break;
            case kRTLStateAuthenticating: case kRTLStateAssociating: case kRTLStateHandshake: self.phase = RTLPhaseConnecting; break;
            case kRTLStateScanning: self.phase = RTLPhaseScanning; break;
            default: self.phase = self.isScanning ? RTLPhaseScanning : RTLPhaseDisconnected;
        }
        struct RTL8723BEDiag d;
        memset(&d, 0, sizeof(d));
        if (rtl_get_diag(conn, &d) == 0) { self.diag = d; self.haveDiag = YES; }
        rtl_close_client(conn);
        [self updateInterface];
    } else {
        rtl_close_client(conn);
    }
    [self notify];
}

- (void)updateInterface {
    char name[32] = {0};
    self.interfaceName = rtl_copy_bsd_name(name, sizeof(name)) == 0 ? ([NSString stringWithUTF8String:name] ?: @"") : @"";
    if (!self.interfaceName.length) return;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return;
    NSString *address = @"";
    uint64_t rx = 0, tx = 0;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_name || strcmp(p->ifa_name, name) != 0 || !p->ifa_addr) continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            char host[NI_MAXHOST] = {0};
            getnameinfo(p->ifa_addr, p->ifa_addr->sa_len, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
            address = [NSString stringWithUTF8String:host] ?: @"";
        } else if (p->ifa_addr->sa_family == AF_LINK && p->ifa_data) {
            const struct if_data *d = (const struct if_data *)p->ifa_data;
            rx = d->ifi_ibytes;
            tx = d->ifi_obytes;
        }
    }
    freeifaddrs(list);
    self.ipAddress = address;
    // Newer macOS leaves if_data input counters at zero for IOKit Ethernet drivers; the driver's own
    // counters (diagnostics v3+) are authoritative when present.
    if (self.haveDiag && self.diag.version >= 3) { rx = self.diag.rx_bytes; tx = self.diag.tx_bytes; }
    NSDate *now = [NSDate date];
    if (_haveCounters && rx >= _lastRx && tx >= _lastTx) {
        double dt = MAX([now timeIntervalSinceDate:_lastCounterTime], 0.1);
        self.rxBps = (double)(rx - _lastRx) * 8 / dt;
        self.txBps = (double)(tx - _lastTx) * 8 / dt;
    }
    _haveCounters = YES;
    _lastRx = rx; _lastTx = tx; _lastCounterTime = now;
}

/// kmutil exists from macOS 11; High Sierra to Catalina only have kextstat.
- (void)readDriverVersion {
    NSString *tool = darwinMajor() >= 20 ? @"/usr/bin/kmutil" : @"/usr/sbin/kextstat";
    NSArray *args = darwinMajor() >= 20
        ? @[@"showloaded", @"--list-only", @"--bundle-identifier", RTL_KEXT_ID]
        : @[@"-l", @"-b", RTL_KEXT_ID];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSTask *task = [[NSTask alloc] init];
        task.launchPath = tool;
        task.arguments = args;
        NSPipe *pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError = [NSPipe pipe];
        NSString *out = @"";
        @try {
            [task launch];
            NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
            [task waitUntilExit];
            out = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
        } @catch (NSException *e) {}
        NSRange r = [out rangeOfString:@"\\([0-9.]+\\)" options:NSRegularExpressionSearch];
        NSString *version = r.location == NSNotFound ? nil : [out substringWithRange:NSMakeRange(r.location + 1, r.length - 2)];
        dispatch_async(dispatch_get_main_queue(), ^{ self.driverVersion = version; [self notify]; });
    });
}

- (void)scan {
    if (!self.driverLoaded || self.isScanning) return;
    self.isScanning = YES;
    self.phase = RTLPhaseScanning;
    [self log:@"Scanning 2.4 GHz channels 1–13…"];
    [self notify];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSMutableArray *found = [NSMutableArray array];
        int rc = -1;
        io_connect_t conn = 0;
        if (rtl_open_client(&conn) == 0) {
            rc = rtl_trigger_scan(conn);
            struct RTL8723BEScanResults raw;
            memset(&raw, 0, sizeof(raw));
            if (rc == 0 && rtl_get_scan_results(conn, &raw) == 0) {
                uint32_t count = MIN(raw.count, (uint32_t)RTL8723BE_MAX_SCAN_APS);
                for (uint32_t i = 0; i < count; i++) {
                    struct RTL8723BEDiscoveredAP *ap = &raw.aps[i];
                    ap->ssid[RTL8723BE_MAX_SSID_LEN] = 0;
                    RTLNetwork *n = [[RTLNetwork alloc] init];
                    n.ssid = [NSString stringWithUTF8String:ap->ssid] ?: @"";
                    n.bssid = formatMAC(ap->bssid);
                    n.bssidData = [NSData dataWithBytes:ap->bssid length:6];
                    n.channel = ap->channel;
                    n.rssi = ap->rssi;
                    n.secured = ap->is_wpa2 != 0;
                    [found addObject:n];
                }
            } else if (rc == 0) {
                rc = -2;
            }
            rtl_close_client(conn);
        }
        [found sortUsingComparator:^NSComparisonResult(RTLNetwork *a, RTLNetwork *b) {
            return a.rssi == b.rssi ? NSOrderedSame : (a.rssi > b.rssi ? NSOrderedAscending : NSOrderedDescending);
        }];
        dispatch_async(dispatch_get_main_queue(), ^{
            self.isScanning = NO;
            if (rc == 0) {
                self.networks = found;
                self.lastScan = [NSDate date];
                [self log:[NSString stringWithFormat:@"Found %lu network%@.", (unsigned long)found.count, found.count == 1 ? @"" : @"s"]];
            } else {
                [self log:[NSString stringWithFormat:@"Scan failed (IOReturn 0x%x).", (unsigned)rc]];
            }
            [self refresh];
        });
    });
}

- (void)joinNetwork:(RTLNetwork *)net password:(NSString *)password {
    self.isBusy = YES;
    [self log:[NSString stringWithFormat:@"Joining %@ (%@, channel %d)…", net.displayName, net.bssid, net.channel]];
    [self notify];
    struct RTL8723BEConnectParams p;
    memset(&p, 0, sizeof(p));
    strlcpy(p.ssid, net.ssid.UTF8String, sizeof(p.ssid));
    strlcpy(p.passphrase, password.UTF8String, sizeof(p.passphrase));
    [net.bssidData getBytes:p.bssid length:6];
    NSString *name = net.displayName;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int rc = -1;
        io_connect_t conn = 0;
        if (rtl_open_client(&conn) == 0) {
            rc = rtl_connect_ap(conn, &p);
            rtl_close_client(conn);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (rc != 0) {
                self.isBusy = NO;
                self.lastAlert = (uint32_t)rc == 0xE00002C2u ? @"The password must be 8–63 characters."
                    : [NSString stringWithFormat:@"The driver refused the request (IOReturn 0x%x).", (unsigned)rc];
                [self log:[@"Join failed: " stringByAppendingString:self.lastAlert]];
                [[NSNotificationCenter defaultCenter] postNotificationName:@"RTLAlert" object:self];
                [self notify];
                return;
            }
            // The 802.11 exchange and WPA2 handshake finish asynchronously in the driver.
            [self waitForJoin:name attempt:0];
        });
    });
}

- (void)waitForJoin:(NSString *)name attempt:(int)attempt {
    [self refresh];
    if (self.phase == RTLPhaseConnected) { self.isBusy = NO; [self notify]; return; }
    if (attempt >= 30 || self.phase == RTLPhaseDisconnected) {
        self.isBusy = NO;
        self.lastAlert = [NSString stringWithFormat:@"Could not join %@. Check the password and that the network is in range.", name];
        [self log:@"Join timed out or was rejected."];
        [[NSNotificationCenter defaultCenter] postNotificationName:@"RTLAlert" object:self];
        [self notify];
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self waitForJoin:name attempt:attempt + 1];
    });
}

- (void)disconnect {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        io_connect_t conn = 0;
        if (rtl_open_client(&conn) == 0) { rtl_disconnect_ap(conn); rtl_close_client(conn); }
        dispatch_async(dispatch_get_main_queue(), ^{ [self refresh]; });
    });
}

- (void)setAntenna:(int)value {
    io_connect_t conn = 0;
    if (rtl_open_client(&conn) != 0) return;
    if (rtl_set_antenna(conn, (uint8_t)value) == 0) {
        self.antenna = value;
        [self log:[NSString stringWithFormat:@"Wi-Fi antenna set to %@.", value == 2 ? @"AUX (S0)" : @"MAIN (S1)"]];
    }
    rtl_close_client(conn);
    [self notify];
}

/// Asks for an administrator password and runs the bundled loader, which picks kmutil or kextload by OS.
- (void)loadDriver {
    NSString *res = [[NSBundle mainBundle] resourcePath];
    if (!res) return;
    NSString *helper = [res stringByAppendingPathComponent:@"load_test_kext.sh"];
    NSString *kext = [res stringByAppendingPathComponent:@"RTL8723BEWiFi.kext"];
    NSString *(^q)(NSString *) = ^NSString *(NSString *s) {
        return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\"'\"'"]];
    };
    NSString *command = [NSString stringWithFormat:@"/bin/bash %@ %@", q(helper), q(kext)];
    command = [command stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    command = [command stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    NSString *source = [NSString stringWithFormat:@"do shell script \"%@\" with administrator privileges", command];
    self.isBusy = YES;
    [self log:@"Loading the driver (administrator password required)…"];
    [self notify];
    dispatch_async(dispatch_get_main_queue(), ^{   // NSAppleScript must run on the main thread
        NSDictionary *error = nil;
        NSAppleEventDescriptor *result = [[[NSAppleScript alloc] initWithSource:source] executeAndReturnError:&error];
        NSString *message = error ? (error[NSAppleScriptErrorMessage] ?: @"Load failed.") : result.stringValue;
        if (message.length) {
            [self log:message];
            if ([message containsString:@"not approved"]) {
                self.lastAlert = darwinMajor() >= 22
                    ? @"macOS needs your approval: open System Settings › Privacy & Security, allow the RTL8723BE driver, then restart."
                    : @"macOS needs your approval: open System Preferences › Security & Privacy, click Allow for the RTL8723BE driver, then restart.";
                [[NSNotificationCenter defaultCenter] postNotificationName:@"RTLAlert" object:self];
            }
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            self.isBusy = NO;
            [self refreshEnvironment];
            [self refresh];
        });
    });
}
@end

// MARK: - UI

static NSTextField *makeLabel(NSString *text, CGFloat size, BOOL bold, NSRect frame) {
    NSTextField *f = [[NSTextField alloc] initWithFrame:frame];
    f.stringValue = text;
    f.editable = NO;
    f.selectable = YES;
    f.bordered = NO;
    f.drawsBackground = NO;
    f.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    f.lineBreakMode = NSLineBreakByWordWrapping;
    return f;
}

static NSButton *makeButton(NSString *title, id target, SEL action, NSRect frame) {
    NSButton *b = [[NSButton alloc] initWithFrame:frame];
    b.title = title;
    b.bezelStyle = NSBezelStyleRounded;
    b.target = target;
    b.action = action;
    return b;
}

static NSImage *statusImage(RTLPhase phase) {
    NSImage *img = [NSImage imageWithSize:NSMakeSize(20, 16) flipped:NO drawingHandler:^BOOL(NSRect r) {
        CGFloat alpha = phase == RTLPhaseConnected ? 1.0 : (phase == RTLPhaseNoDriver ? 0.35 : 0.65);
        [[NSColor colorWithCalibratedWhite:0 alpha:alpha] set];
        NSPoint c = NSMakePoint(10, 2.5);
        NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 1.6, c.y - 1.6, 3.2, 3.2)];
        [dot fill];
        for (int i = 1; i <= 3; i++) {
            NSBezierPath *arc = [NSBezierPath bezierPath];
            [arc appendBezierPathWithArcWithCenter:c radius:3.2 * i + 1 startAngle:45 endAngle:135];
            arc.lineWidth = 1.7;
            arc.lineCapStyle = NSRoundLineCapStyle;
            if (phase == RTLPhaseDisconnected && i == 3) { CGFloat dash[2] = {2, 2}; [arc setLineDash:dash count:2 phase:0]; }
            [arc stroke];
        }
        if (phase == RTLPhaseNoDriver) {
            NSDictionary *a = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:9], NSForegroundColorAttributeName: [NSColor blackColor]};
            [@"!" drawAtPoint:NSMakePoint(16, 5) withAttributes:a];
        }
        return YES;
    }];
    img.template = YES;
    return img;
}

@interface RTLAppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate, NSWindowDelegate>
@end

@implementation RTLAppDelegate {
    RTLController *_c;
    NSWindow *_window;
    NSTextField *_title, *_subline, *_setup;
    NSButton *_primary, *_load, *_join;
    NSTableView *_table;
    NSTextView *_logView;
    NSSegmentedControl *_antenna;
    NSTextField *_driverInfo, *_scanInfo;
    NSStatusItem *_statusItem;
    BOOL _alertShowing;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    _c = [[RTLController alloc] init];
    [self buildMenus];
    [self buildWindow];
    [self buildStatusItem];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(update) name:RTLStateChanged object:_c];
    [nc addObserver:self selector:@selector(updateLog) name:@"RTLLogChanged" object:_c];
    [nc addObserver:self selector:@selector(showAlert) name:@"RTLAlert" object:_c];
    [self update];
    [self updateLog];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)flag {
    [self showWindow:nil];
    return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return NO; }

- (void)showWindow:(id)sender {
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// MARK: Menus

- (NSMenuItem *)item:(NSString *)title action:(SEL)action key:(NSString *)key target:(id)target {
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    i.target = target;
    return i;
}

- (void)buildMenus {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *app = [[NSMenu alloc] initWithTitle:@"RTL8723BE Wi-Fi"];
    [app addItem:[self item:@"About RTL8723BE Wi-Fi" action:@selector(showAbout:) key:@"" target:self]];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:[self item:@"Hide RTL8723BE Wi-Fi" action:@selector(hide:) key:@"h" target:NSApp]];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:[self item:@"Quit RTL8723BE Wi-Fi" action:@selector(terminate:) key:@"q" target:NSApp]];
    appItem.submenu = app;

    NSMenuItem *fileItem = [[NSMenuItem alloc] init];
    [bar addItem:fileItem];
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [file addItem:[self item:@"Scan for Networks" action:@selector(scan:) key:@"r" target:self]];
    [file addItem:[self item:@"Close Window" action:@selector(performClose:) key:@"w" target:nil]];
    fileItem.submenu = file;

    // Without an Edit menu, Cmd-V does not work in the password field.
    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [bar addItem:editItem];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItem:[self item:@"Cut" action:@selector(cut:) key:@"x" target:nil]];
    [edit addItem:[self item:@"Copy" action:@selector(copy:) key:@"c" target:nil]];
    [edit addItem:[self item:@"Paste" action:@selector(paste:) key:@"v" target:nil]];
    [edit addItem:[self item:@"Select All" action:@selector(selectAll:) key:@"a" target:nil]];
    editItem.submenu = edit;

    NSMenuItem *winItem = [[NSMenuItem alloc] init];
    [bar addItem:winItem];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItem:[self item:@"Minimize" action:@selector(performMiniaturize:) key:@"m" target:nil]];
    [win addItem:[self item:@"RTL8723BE Wi-Fi" action:@selector(showWindow:) key:@"" target:self]];
    winItem.submenu = win;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = win;
}

- (void)showAbout:(id)sender {
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSDictionary *attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:11], NSParagraphStyleAttributeName: para};
    NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
        initWithString:@"Made by Issen Software Group. Hardware support ported from the Linux rtlwifi driver.\n" attributes:attrs];
    NSMutableDictionary *link = [attrs mutableCopy];
    link[NSLinkAttributeName] = [NSURL URLWithString:RTL_WEBSITE_URL];
    [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"issen.kurokamicorp.com" attributes:link]];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{@"Credits": credits}];
}

// MARK: Window

- (void)buildWindow {
    const CGFloat W = 760, H = 580;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"RTL8723BE Wireless Utility";
    _window.minSize = NSMakeSize(640, 500);
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    [_window center];
    NSView *content = _window.contentView;

    // Header (pinned to the top)
    _title = makeLabel(@"", 22, YES, NSMakeRect(20, H - 50, W - 40, 30));
    _title.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    _subline = makeLabel(@"", 13, NO, NSMakeRect(20, H - 76, W - 40, 20));
    _subline.textColor = [NSColor secondaryLabelColor];
    _subline.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    _setup = makeLabel(@"", 12, NO, NSMakeRect(20, H - 160, W - 40, 80));
    _setup.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    _primary = makeButton(@"Scan", self, @selector(primaryAction:), NSMakeRect(20, H - 200, 110, 30));
    _primary.keyEquivalent = @"\r";
    _primary.autoresizingMask = NSViewMinYMargin;
    _load = makeButton(@"Load Driver…", self, @selector(loadDriver:), NSMakeRect(140, H - 200, 130, 30));
    _load.autoresizingMask = NSViewMinYMargin;
    _join = makeButton(@"Join…", self, @selector(joinSelected:), NSMakeRect(280, H - 200, 90, 30));
    _join.autoresizingMask = NSViewMinYMargin;
    for (NSView *v in @[_title, _subline, _setup, _primary, _load, _join]) [content addSubview:v];

    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 12, W - 24, H - 224)];
    tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:tabs];

    // Networks
    NSTabViewItem *netTab = [[NSTabViewItem alloc] initWithIdentifier:@"networks"];
    netTab.label = @"Networks";
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, W - 50, H - 270)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSArray *cols = @[@[@"name", @"Network", @300], @[@"signal", @"Signal", @110], @[@"channel", @"Channel", @70],
                      @[@"security", @"Security", @90], @[@"bssid", @"BSSID", @140]];
    for (NSArray *c in cols) {
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:c[0]];
        col.title = c[1];
        col.width = [c[2] doubleValue];
        [_table addTableColumn:col];
    }
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(joinSelected:);
    scroll.documentView = _table;
    netTab.view = scroll;
    [tabs addTabViewItem:netTab];

    // Activity
    NSTabViewItem *logTab = [[NSTabViewItem alloc] initWithIdentifier:@"activity"];
    logTab.label = @"Activity";
    NSScrollView *logScroll = [[NSScrollView alloc] initWithFrame:scroll.frame];
    logScroll.hasVerticalScroller = YES;
    logScroll.borderType = NSBezelBorder;
    logScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:logScroll.bounds];
    _logView.editable = NO;
    _logView.font = [NSFont userFixedPitchFontOfSize:11];
    _logView.autoresizingMask = NSViewWidthSizable;
    logScroll.documentView = _logView;
    logTab.view = logScroll;
    [tabs addTabViewItem:logTab];

    // Settings
    NSTabViewItem *setTab = [[NSTabViewItem alloc] initWithIdentifier:@"settings"];
    setTab.label = @"Settings";
    NSView *sv = [[NSView alloc] initWithFrame:scroll.frame];
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    CGFloat y = H - 270 - 30;
    NSTextField *antLabel = makeLabel(@"Wi-Fi antenna", 13, YES, NSMakeRect(16, y, 200, 20));
    antLabel.autoresizingMask = NSViewMinYMargin;
    _antenna = [NSSegmentedControl segmentedControlWithLabels:@[@"Main (S1)", @"Aux (S0)"]
                                                 trackingMode:NSSegmentSwitchTrackingSelectOne
                                                       target:self action:@selector(antennaChanged:)];
    _antenna.frame = NSMakeRect(16, y - 30, 200, 24);
    _antenna.autoresizingMask = NSViewMinYMargin;
    NSTextField *antHint = makeLabel(@"The first scan after the driver loads measures both RF paths and keeps the better one. "
                                     @"Many HP boards have their single antenna on Aux.", 11, NO, NSMakeRect(16, y - 80, W - 70, 40));
    antHint.textColor = [NSColor secondaryLabelColor];
    antHint.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    _driverInfo = makeLabel(@"", 12, NO, NSMakeRect(16, y - 150, W - 70, 60));
    _driverInfo.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    NSButton *privacy = makeButton(@"Privacy & Security…", self, @selector(openPrivacy:), NSMakeRect(16, y - 190, 170, 28));
    NSButton *donate = makeButton(@"Donate via PayPal", self, @selector(donate:), NSMakeRect(196, y - 190, 160, 28));
    NSButton *releases = makeButton(@"Check for Updates…", self, @selector(openReleases:), NSMakeRect(366, y - 190, 170, 28));
    for (NSView *v in @[privacy, donate, releases]) v.autoresizingMask = NSViewMinYMargin;
    NSTextField *about = makeLabel(@"Made by Issen Software Group. The app is MIT-licensed; the driver is GPL-2.0-only, ported from the "
                                   @"Linux rtlwifi rtl8723be driver. Firmware © Realtek Semiconductor Corp.",
                                   11, NO, NSMakeRect(16, y - 250, W - 70, 40));
    about.textColor = [NSColor secondaryLabelColor];
    about.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    for (NSView *v in @[antLabel, _antenna, antHint, _driverInfo, privacy, donate, releases, about]) [sv addSubview:v];
    setTab.view = sv;
    [tabs addTabViewItem:setTab];
}

- (void)buildStatusItem {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    [self setStatusImage:statusImage(RTLPhaseNoDriver)];
    NSMenu *menu = [[NSMenu alloc] init];
    menu.delegate = self;
    _statusItem.menu = menu;
}

- (void)setStatusImage:(NSImage *)image {
    if ([_statusItem respondsToSelector:@selector(button)]) _statusItem.button.image = image;   // 10.10+
    else _statusItem.image = image;
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSString *head = _c.phase == RTLPhaseConnected ? _c.ssid : [self phaseLabel];
    [menu addItem:[self item:head action:nil key:@"" target:nil]];
    if (_c.phase == RTLPhaseConnected) {
        [menu addItem:[self item:[NSString stringWithFormat:@"Down %@   Up %@", formatBitrate(_c.rxBps), formatBitrate(_c.txBps)]
                          action:nil key:@"" target:nil]];
        [menu addItem:[self item:[NSString stringWithFormat:@"%@  %@", _c.interfaceName.length ? _c.interfaceName : @"—",
                                  _c.ipAddress.length ? _c.ipAddress : @"no IP yet"] action:nil key:@"" target:nil]];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    if (!_c.driverLoaded) [menu addItem:[self item:@"Load Driver…" action:@selector(loadDriver:) key:@"" target:self]];
    else if (_c.phase == RTLPhaseConnected) [menu addItem:[self item:@"Disconnect" action:@selector(primaryAction:) key:@"" target:self]];
    else [menu addItem:[self item:@"Scan for Networks" action:@selector(scan:) key:@"" target:self]];
    [menu addItem:[self item:@"Open RTL8723BE Wi-Fi" action:@selector(showWindow:) key:@"" target:self]];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:[self item:@"Quit" action:@selector(terminate:) key:@"" target:NSApp]];
}

// MARK: State -> UI

- (NSString *)phaseLabel {
    switch (_c.phase) {
        case RTLPhaseNoDriver: return @"Driver not loaded";
        case RTLPhaseDisconnected: return @"Not connected";
        case RTLPhaseScanning: return @"Scanning…";
        case RTLPhaseConnecting: return @"Connecting…";
        case RTLPhaseConnected: return @"Connected";
    }
    return @"";
}

- (void)update {
    RTLController *c = _c;
    _title.stringValue = c.phase == RTLPhaseConnected ? c.ssid : [self phaseLabel];
    switch (c.phase) {
        case RTLPhaseConnected: {
            NSString *ip = c.ipAddress.length ? c.ipAddress : @"waiting for DHCP…";
            _subline.stringValue = [NSString stringWithFormat:@"Channel %d · %d dBm · %@ antenna · %@ · %@ · up %@ / down %@ · %@",
                                    c.channel, c.rssi, c.antenna == 2 ? @"AUX" : @"MAIN", c.interfaceName.length ? c.interfaceName : @"—",
                                    ip, formatBitrate(c.txBps), formatBitrate(c.rxBps),
                                    c.connectedSince ? formatDuration(-[c.connectedSince timeIntervalSinceNow]) : @""];
            break;
        }
        case RTLPhaseConnecting: _subline.stringValue = @"Authenticating and running the WPA2 handshake…"; break;
        case RTLPhaseScanning: _subline.stringValue = @"Listening on 2.4 GHz channels 1–13…"; break;
        case RTLPhaseDisconnected:
            _subline.stringValue = c.networks.count ? [NSString stringWithFormat:@"%lu networks nearby. Pick one and click Join.", (unsigned long)c.networks.count]
                                                    : @"Click Scan to look for nearby 2.4 GHz networks.";
            break;
        case RTLPhaseNoDriver: _subline.stringValue = @"The RTL8723BE driver is not loaded."; break;
    }
    _setup.hidden = c.driverLoaded;
    if (!c.driverLoaded) {
        NSString *reason = !c.hardwarePresent ? @"No RTL8723BE card was found, so there is nothing to load the driver for."
            : (!c.bootArgPresent ? @"This Mac was started without rtl8723be_experimental=1 in boot-args; restart with an OpenCore EFI that includes it."
                                  : @"Load the driver after each login. The first time macOS asks you to allow it in Security & Privacy; allow it, restart, and load it again.");
        _setup.stringValue = [NSString stringWithFormat:@"%@  Wi-Fi card detected (pci10ec,b723)\n%@  Boot flag rtl8723be_experimental=1 set\n%@  Driver running\n\n%@",
                              c.hardwarePresent ? @"✓" : @"✗", c.bootArgPresent ? @"✓" : @"✗",
                              c.driverLoaded ? @"✓" : @"✗", reason];
    }
    _load.hidden = c.driverLoaded;
    _load.enabled = !c.isBusy && c.hardwarePresent && c.bootArgPresent;
    _primary.hidden = !c.driverLoaded;
    _primary.title = c.phase == RTLPhaseConnected ? @"Disconnect" : @"Scan";
    _primary.enabled = !c.isScanning && !c.isBusy;
    _join.hidden = !c.driverLoaded;
    _join.enabled = c.driverLoaded && !c.isBusy && _table.selectedRow >= 0;
    _antenna.selectedSegment = c.antenna == 2 ? 1 : 0;
    _antenna.enabled = c.driverLoaded;
    _driverInfo.stringValue = c.driverLoaded
        ? [NSString stringWithFormat:@"RTL8723BEWiFi.kext %@ is loaded.\nHardware MAC %@%@", c.driverVersion ? [@"v" stringByAppendingString:c.driverVersion] : @"",
           c.macAddress,
           c.haveDiag ? [NSString stringWithFormat:@"   Firmware v%u.%u", c.diag.fw_version >> 16, c.diag.fw_version & 0xFFFF] : @""]
        : @"The driver is not loaded.";
    [_table reloadData];
    [self setStatusImage:statusImage(c.phase)];
}

- (void)updateLog {
    _logView.string = [_c.logLines componentsJoinedByString:@"\n"];
    [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showAlert {
    if (_alertShowing || !_c.lastAlert) return;
    _alertShowing = YES;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"RTL8723BE Wi-Fi";
    a.informativeText = _c.lastAlert;
    [a runModal];
    _c.lastAlert = nil;
    _alertShowing = NO;
}

// MARK: Actions

- (void)scan:(id)sender { [_c scan]; }

- (void)primaryAction:(id)sender {
    if (_c.phase == RTLPhaseConnected) [_c disconnect]; else [_c scan];
}

- (void)loadDriver:(id)sender { [_c loadDriver]; }

- (void)antennaChanged:(NSSegmentedControl *)sender { [_c setAntenna:sender.selectedSegment == 1 ? 2 : 1]; }

- (void)joinSelected:(id)sender {
    NSInteger row = _table.clickedRow >= 0 && sender == _table ? _table.clickedRow : _table.selectedRow;
    if (!_c.driverLoaded || row < 0 || row >= (NSInteger)_c.networks.count) return;
    RTLNetwork *net = _c.networks[row];
    if (!net.secured) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Unsupported network";
        a.informativeText = @"Only WPA2 Personal (WPA2-PSK) networks can be joined. Open, WEP, WPA3 and enterprise networks are not supported.";
        [a runModal];
        return;
    }
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Join “%@”", net.displayName];
    a.informativeText = @"Enter the network password (8–63 characters).";
    NSSecureTextField *field = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    a.accessoryView = field;
    [a addButtonWithTitle:@"Join"];
    [a addButtonWithTitle:@"Cancel"];
    a.window.initialFirstResponder = field;
    if ([a runModal] == NSAlertFirstButtonReturn) [_c joinNetwork:net password:field.stringValue];
}

- (void)openPrivacy:(id)sender {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security"]];
}
- (void)donate:(id)sender { [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:RTL_DONATE_URL]]; }
- (void)openReleases:(id)sender { [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:RTL_RELEASES_URL]]; }

// MARK: Table

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)_c.networks.count; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    RTLNetwork *n = _c.networks[row];
    NSString *ident = col.identifier;
    if ([ident isEqualToString:@"name"]) return n.displayName;
    if ([ident isEqualToString:@"signal"]) {
        int bars = n.rssi >= -55 ? 4 : (n.rssi >= -67 ? 3 : (n.rssi >= -78 ? 2 : 1));
        return [NSString stringWithFormat:@"%@%@  %d dBm", [@"" stringByPaddingToLength:bars withString:@"▮" startingAtIndex:0],
                [@"" stringByPaddingToLength:4 - bars withString:@"▯" startingAtIndex:0], n.rssi];
    }
    if ([ident isEqualToString:@"channel"]) return [NSString stringWithFormat:@"%d", n.channel];
    if ([ident isEqualToString:@"security"]) return n.secured ? @"WPA2" : @"Unsupported";
    return n.bssid;
}

- (BOOL)tableView:(NSTableView *)tv shouldEditTableColumn:(NSTableColumn *)col row:(NSInteger)row { return NO; }
- (void)tableViewSelectionDidChange:(NSNotification *)n { _join.enabled = _c.driverLoaded && !_c.isBusy && _table.selectedRow >= 0; }

@end

int main(int argc, char **argv) {
    execModernIfSupported(argv);
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        RTLAppDelegate *delegate = [[RTLAppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
