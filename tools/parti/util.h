#define SEP		"- - - - - - - - - - - - - - - -"

char *cname(void *buf, int len);

unsigned read_byte(void *buf);
unsigned read_word_le(void *buf);
unsigned read_word_be(void *buf);
unsigned read_dword_le(void *buf);
unsigned read_dword_be(void *buf);
uint64_t read_qword_le(void *buf);
uint64_t read_qword_be(void *buf);

void log_info(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

typedef struct {
  unsigned verbose;
  struct {
    unsigned raw:1;
  } show;
  char *export_file;
  unsigned json:1;
  unsigned mkisofs:1;
  unsigned xorriso:1;
} opt_t;

extern opt_t opt;
