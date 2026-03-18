#include <json-c/json.h>

// internal block size, fixed
#define DISK_CHUNK_SIZE		512
// resize internal chunk list by that amount, if needed
#define DISK_CHUNKS_EXTRA	256
// maximum number of chunks to store in internal cache (cache size = 512 MiB)
#define DISK_MAX_CHUNKS		1024*1024

typedef struct {
  uint64_t nr;
  uint8_t *data;
} disk_chunk_t;

typedef struct {
  char *name;
  int fd;
  unsigned index;
  unsigned heads;
  unsigned sectors;
  unsigned cylinders;
  uint64_t size_in_bytes;
  unsigned block_size;
  unsigned grub_used:1;
  unsigned isolinux_used:1;
  struct {
    disk_chunk_t *list;
    unsigned len, max;
  } chunks;
  json_object *json_disk;
  json_object *json_current;
} disk_t;

extern unsigned disk_list_size;
extern disk_t *disk_list;

int disk_read(disk_t *disk, void *buf, uint64_t sector, unsigned cnt);
int disk_read_single(disk_t *disk, void *buffer, uint64_t block_nr);

int disk_cache_read(disk_t *disk, disk_chunk_t *chunk);
int disk_cache_store(disk_t *disk, disk_chunk_t *chunk);
int disk_cache_dump(disk_t *disk, disk_chunk_t *chunk, FILE *file);

unsigned disk_find_chunk(disk_t *disk, uint64_t chunk_nr, int *match);

int disk_export(disk_t *disk, char *file_name);
int disk_to_fd(disk_t *disk, uint64_t offset);
void disk_add_to_list(disk_t *disk);
void disk_init(char *file_name);
void disk_import(char *file_name);
