#include "hashtable.h"
#include "events.h"
#include "windows.h"
#include "mach.h"
#include <stdio.h>

pid_t g_pid;
struct table g_windows;
struct mach_server g_mach_server;
struct settings g_settings = { .active_window_color = 0xffe1e3e4,
                               .inactive_window_color = 0xff494d64,
                               .border_width = 4.f,
                               .border_style = BORDER_STYLE_ROUND  };

static TABLE_HASH_FUNC(hash_windows)
{
  return *(uint32_t *) key;
}

static TABLE_COMPARE_FUNC(cmp_windows)
{
  return *(uint32_t *) key_a == *(uint32_t *) key_b;
}

static void acquire_lockfile(void) {
  char *user = getenv("USER");
  if (!user) {
    error("JankyBorders: 'env USER' not set! abort..\n");
  }

  char lock_file[4096] = {};
  snprintf(lock_file, sizeof(lock_file), "/tmp/JankyBorders_%s.lock", user);

  int handle = open(lock_file, O_CREAT | O_WRONLY, 0600);
  if (handle == -1) {
    error("JankyBorders: could not create lock-file! abort..\n");
  }

  struct flock lockfd = {
    .l_start  = 0,
    .l_len    = 0,
    .l_pid    = g_pid,
    .l_type   = F_WRLCK,
    .l_whence = SEEK_SET
  };

  if (fcntl(handle, F_SETLK, &lockfd) == -1) {
    error("JankyBorders: could not acquire lock-file! already running?\n");
  }
}

void event_callback(CFMachPortRef port, void* message, CFIndex size, void* context) {
  int cid = SLSMainConnectionID();
  CGEventRef event = SLEventCreateNextEvent(cid);
  if (!event) return;
  do {
    CGEventType type = CGEventGetType(event);
    CFRelease(event);
    event = SLEventCreateNextEvent(cid);
  } while (event != NULL);
}

static uint32_t parse_settings(struct settings* settings, int count, char** arguments) {
  uint32_t update_mask = 0;
  for (int i = 0; i < count; i++) {
    if (sscanf(arguments[i],
               "active_color=0x%x",
               &settings->active_window_color) == 1) {
      update_mask |= BORDER_UPDATE_MASK_ACTIVE;
      continue;
    }
    else if (sscanf(arguments[i],
             "inactive_color=0x%x",
             &settings->inactive_window_color) == 1) {
      update_mask |= BORDER_UPDATE_MASK_INACTIVE;
      continue;
    }
    else if (sscanf(arguments[i], "width=%f", &settings->border_width) == 1) {
      update_mask |= BORDER_UPDATE_MASK_ALL;
      continue;
    }
    else if (sscanf(arguments[i], "style=%c", &settings->border_style) == 1) {
      update_mask |= BORDER_UPDATE_MASK_ALL;
      continue;
    } else {
      printf("[?] Borders: Invalid argument '%s'\n", arguments[i]);
    }
  }
  return update_mask;
}

static void message_handler(void* data, uint32_t len) {
  char* message = data;
  uint32_t update_mask = 0;
  while(message && *message) {
    update_mask |= parse_settings(&g_settings, 1, &message);
    message += strlen(message) + 1;
  }
  if (update_mask & BORDER_UPDATE_MASK_ALL) {
    windows_window_update_all(&g_windows);
  } else if (update_mask & BORDER_UPDATE_MASK_ACTIVE) {
    windows_window_update_active(&g_windows);
  } else if (update_mask & BORDER_UPDATE_MASK_INACTIVE) {
    windows_window_update_inactive(&g_windows);
  }
}

static void send_args_to_server(mach_port_t port, int argc, char** argv) {
  int message_length = argc;
  int argl[argc];

  for (int i = 1; i < argc; i++) {
    argl[i] = strlen(argv[i]);
    message_length += argl[i] + 1;
  }

  char message[(sizeof(char) * message_length)];
  char* temp = message;

  for (int i = 1; i < argc; i++) {
    memcpy(temp, argv[i], argl[i]);
    temp += argl[i];
    *temp++ = '\0';
  }
  *temp++ = '\0';

  mach_send_message(port, message, message_length);
}

int main(int argc, char** argv) {
  uint32_t update_mask = 0;
  if (argc > 1) {
    update_mask = parse_settings(&g_settings, argc - 1, argv + 1);
  }

  if (update_mask) {
    mach_port_t port = mach_get_bs_port("git.felix.borders");
    if (port) {
      send_args_to_server(port, argc, argv);
      return 0;
    }
  }

  pid_for_task(mach_task_self(), &g_pid);
  acquire_lockfile();
  table_init(&g_windows, 1024, hash_windows, cmp_windows);
  events_register();

  SLSWindowManagementBridgeSetDelegate(NULL);
  mach_port_t port;
  CGError error = SLSGetEventPort(SLSMainConnectionID(), &port);
  CFMachPortRef cf_mach_port = NULL;
  CFRunLoopSourceRef source = NULL;
  if (error == kCGErrorSuccess) {
    CFMachPortRef cf_mach_port = CFMachPortCreateWithPort(NULL,
                                                          port,
                                                          event_callback,
                                                          NULL,
                                                          false          );

    _CFMachPortSetOptions(cf_mach_port, 0x40);
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL,
                                                              cf_mach_port,
                                                              0            );

    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRelease(cf_mach_port);
    CFRelease(source);
  }

  windows_add_existing_windows(&g_windows);
  mach_server_begin(&g_mach_server, message_handler);
  CFRunLoopRun();

  return 0;
}

