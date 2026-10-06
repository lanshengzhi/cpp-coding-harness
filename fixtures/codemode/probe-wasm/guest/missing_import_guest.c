// guest/missing_import_guest.c — guest that requires a host import the probe
// host does not provide.
//
// probe #868 uses it to measure fail-closed linking: an undefined import must
// abort instantiation, not silently resolve to a no-op.

__attribute__((import_module("env"), import_name("fs_read_file")))
extern int fs_read_file(const char *path, int len);

__attribute__((export_name("run")))
int run(void) {
  return fs_read_file("/etc/passwd", 11);
}
