#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/fs.h>   /* BLKGETSIZE64 */

#include "util.h"
#include "disk.h" 

extern json_object *json_root;

unsigned disk_list_size;
disk_t *disk_list;

int disk_read(disk_t *disk, void *buffer, uint64_t block_nr, unsigned count)
{
  unsigned factor = disk->block_size / DISK_CHUNK_SIZE;

  // fprintf(stderr, "read: %llu - %u (factor = %u)\n", (unsigned long long) block_nr, count, factor);

  uint64_t chunk_nr = block_nr * factor;

  count *= factor;

  for(unsigned u = 0; u < count; u++, chunk_nr++, buffer += DISK_CHUNK_SIZE) {
    // fprintf(stderr, "read request: disk %u, addr %08"PRIx64"\n", disk->index, chunk_nr * DISK_CHUNK_SIZE);
    if(disk_cache_read(disk, &(disk_chunk_t) { .nr = chunk_nr, .data = buffer })) {
      int err = disk_read_single(disk, buffer, chunk_nr);
      if(err) return err;
      err = disk_cache_store(disk, &(disk_chunk_t) { .nr = chunk_nr, .data = buffer });
      if(err) return err;
    }
  }

  return 0;
}


int disk_read_single(disk_t *disk, void *buffer, uint64_t block_nr)
{
  if(disk->fd == -1) {
    // fprintf(stderr, "cache miss: disk %u, addr %08"PRIx64"\n", disk->index, block_nr * DISK_CHUNK_SIZE);
    memset(buffer, 0, DISK_CHUNK_SIZE);

    return 0;
  }

  // fprintf(stderr, "read: %llu[%llu]\n", (unsigned long long) block_nr, (unsigned long long) disk->size);

  off_t offset = block_nr * DISK_CHUNK_SIZE;

  if(lseek(disk->fd, offset, SEEK_SET) != offset) {
    fprintf(stderr, "sector %"PRIu64" not found\n", block_nr);

    return 2;
  }

  if(read(disk->fd, buffer, DISK_CHUNK_SIZE) != DISK_CHUNK_SIZE) {
    fprintf(stderr, "error reading sector %"PRIu64"\n", block_nr);

    return 3;
  }

  return 0;
}


int disk_cache_read(disk_t *disk, disk_chunk_t *chunk)
{
  if(!chunk || !chunk->data || chunk->nr == UINT64_MAX) return 1;

  int match;
  unsigned u = disk_find_chunk(disk, chunk->nr, &match);

  if(!match) return 2;

  memcpy(chunk->data, disk->chunks.list[u].data, DISK_CHUNK_SIZE);

  return 0;
}


int disk_cache_store(disk_t *disk, disk_chunk_t *chunk)
{
  if(!chunk || !chunk->data || chunk->nr == UINT64_MAX) return 1;

  int match;
  unsigned u = disk_find_chunk(disk, chunk->nr, &match);

  if(!match) {
    if(disk->chunks.len >= DISK_MAX_CHUNKS) return 2;
    void *buffer = malloc(DISK_CHUNK_SIZE);
    if(!buffer) return 3;
    memcpy(buffer, chunk->data, DISK_CHUNK_SIZE);
    disk->chunks.len++;
    if(disk->chunks.len > disk->chunks.max) {
      disk->chunks.max += DISK_CHUNKS_EXTRA;
      disk->chunks.list = realloc(disk->chunks.list, disk->chunks.max * sizeof (disk_chunk_t));
      if(disk->chunks.list == NULL) {
        free(buffer);
        disk->chunks.len = disk->chunks.max = 0;
        return 4;
      }
    }
    if(u + 1 < disk->chunks.len) {
      memmove(disk->chunks.list + u + 1, disk->chunks.list + u, sizeof (disk_chunk_t) * (disk->chunks.len - u - 1));
    }
    disk->chunks.list[u].nr = chunk->nr;
    disk->chunks.list[u].data = buffer;
  }

  return 0;
}


// Binary search.
// If matched, return value is index that matched.
// If no match, return value is position at which to insert new value (may
// point at position right after end of the list).
unsigned disk_find_chunk(disk_t *disk, uint64_t chunk_nr, int *match)
{
  *match = 0;

  disk_chunk_t *chunk_list = disk->chunks.list;

  unsigned u_start = 0;
  unsigned u_end = disk->chunks.len;
  unsigned u = 0;

  while(u_end > u_start) {
    u = (u_end + u_start) / 2;

    int64_t i = chunk_nr - chunk_list[u].nr;

    if(i == 0) {
      *match = 1;
      break;
    }

    if(u_end == u_start + 1) {
      if(i > 0) u++;
      break;
    }

    if(i > 0) {
      if(u_end == u + 1) {
        if(i > 0) u++;
        break;
      }
      u_start = u;
    }
    else {
      u_end = u;
    }
  }

  return u;
}


int disk_export(disk_t *disk, char *file_name)
{
  FILE *f = stdout;

  if(strcmp(file_name, "-")) {
    f = fopen(file_name, "a");
    if(!f) {
      perror(file_name);
      return 1;
    }
  }

  fprintf(f, "# disk %u, size = %"PRIu64"\n", disk->index, disk->size_in_bytes);

  for(unsigned u = 0; u < disk->chunks.len; u++) {
    disk_cache_dump(disk, disk->chunks.list + u, f);
  }

  if(f != stdout) fclose(f);

  return 0;
}


int disk_to_fd(disk_t *disk, uint64_t offset)
{
  int fd = syscall(SYS_memfd_create, "", 0);

  if(fd == -1) return 0;

  for(unsigned u = 0; u < disk->chunks.len; u++) {
    if(disk->chunks.list[u].nr * DISK_CHUNK_SIZE >= offset) {
      lseek(fd, disk->chunks.list[u].nr * DISK_CHUNK_SIZE - offset, SEEK_SET);
      write(fd, disk->chunks.list[u].data, DISK_CHUNK_SIZE);
    }
  }

  lseek(fd, 0, SEEK_SET);

  return fd;
}


int disk_cache_dump(disk_t *disk, disk_chunk_t *chunk, FILE *file)
{
  if(!file) return 1;

  uint8_t all_zeros[16] = {};
  uint8_t *data = chunk->data;

  uint64_t max_addr = disk->size_in_bytes - 1;
  unsigned address_digits = 0;
  while(max_addr >>= 4) address_digits++;
  if(address_digits < 4) address_digits = 4;

  for(unsigned u = 0; u < DISK_CHUNK_SIZE; u += 16) {
    if(memcmp(data + u, &all_zeros, 16)) {
      fprintf(file, "%0*"PRIx64" ", address_digits, chunk->nr * DISK_CHUNK_SIZE + u);
      for(unsigned u1 = 0; u1 < 16; u1++) {
        fprintf(file, " %02x", data[u + u1]);
      }
      fprintf(file, "  ");
      for(unsigned u1 = 0; u1 < 16; u1++) {
        fprintf(file, "%c", data[u + u1] >= 32 && data[u + u1] < 0x7f ? data[u + u1] : '.');
      }
      fprintf(file, "\n");
    }
  }

  return 0;
}


void disk_add_to_list(disk_t *disk)
{
  json_object *json;

  disk_list = reallocarray(disk_list, disk_list_size + 1, sizeof *disk_list);
  disk_list[disk_list_size] = *disk;
  disk_list[disk_list_size].index = disk_list_size;
  disk_list[disk_list_size].json_disk =
    disk_list[disk_list_size].json_current = json = json_object_new_object();
  json_object_array_add(json_root, json);

  disk_list_size++;

  log_info("%s: %"PRIu64" bytes\n", disk->name, disk->size_in_bytes);

  json_object *json_device = json_object_new_object();

  json_object_object_add(json, "device", json_device);
  json_object_object_add(json_device, "file_name", json_object_new_string(disk->name));
  json_object_object_add(json_device, "block_size", json_object_new_int(disk->block_size));
  json_object_object_add(json_device, "size", json_object_new_int64(disk->size_in_bytes / disk->block_size));
}


void disk_init(char *file_name)
{
  struct stat sbuf;
  disk_t disk = { .block_size = DISK_CHUNK_SIZE };

  disk.fd = open(file_name, O_RDONLY | O_LARGEFILE);

  if(disk.fd == -1) {
    perror(file_name);
    exit(1);
  }

  disk.name = strdup(file_name);

  if(!fstat(disk.fd, &sbuf)) disk.size_in_bytes = sbuf.st_size;
  if(!disk.size_in_bytes && ioctl(disk.fd, BLKGETSIZE64, &disk.size_in_bytes)) disk.size_in_bytes = 0;

  disk_add_to_list(&disk);
}


void disk_import(char *file_name)
{
  FILE *file = fopen(file_name, "r");

  if(!file) {
    perror(file_name);
    exit(1);
  }

  char *line = NULL;
  size_t line_len = 0;
  unsigned line_nr = 0;

  disk_t disk = { .fd = -1 };

  uint8_t buffer[DISK_CHUNK_SIZE];
  uint64_t current_chunk_nr = UINT64_MAX;

  while(getline(&line, &line_len, file) > 0) {
    line_nr++;
    unsigned index;
    uint64_t size;
    uint64_t addr;
    uint8_t line_data[16];
    if(sscanf(line, "# disk %u, size = %"SCNu64"", &index, &size) == 2) {
      if(disk.name) {
        if(current_chunk_nr != UINT64_MAX) disk_cache_store(&disk, &(disk_chunk_t) { .nr = current_chunk_nr, .data = buffer });
        disk_add_to_list(&disk);
        disk = (disk_t) { .index = disk_list_size };
        current_chunk_nr = UINT64_MAX;
      }
      asprintf(&disk.name, "%s#%u", file_name, index);
      disk.size_in_bytes = size;
      disk.block_size = DISK_CHUNK_SIZE;
    }
    else if(
      sscanf(line,
        "%"SCNx64" %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
        &addr,
        line_data, line_data + 1, line_data + 2, line_data + 3,
        line_data + 4, line_data + 5, line_data + 6, line_data + 7,
        line_data + 8, line_data + 9, line_data + 10, line_data + 11,
        line_data + 12, line_data + 13, line_data + 14, line_data + 15
      ) == 17 &&
      !(addr & 0xf) &&
      addr <= disk.size_in_bytes + 16
    ) {
      uint64_t chunk_nr = addr / DISK_CHUNK_SIZE;
      if(chunk_nr != current_chunk_nr) {
        if(current_chunk_nr != UINT64_MAX) disk_cache_store(&disk, &(disk_chunk_t) { .nr = current_chunk_nr, .data = buffer });
        current_chunk_nr = chunk_nr;
        memset(buffer, 0, sizeof buffer);
      }
      memcpy(buffer + (addr % DISK_CHUNK_SIZE), line_data, 16);
    }
    else {
      fprintf(stderr, "%s: line %u: invalid import data: %s\n", file_name, line_nr, line);
      exit(1);
    }
  }

  free(line);

  if(disk.name) {
    if(current_chunk_nr != UINT64_MAX) disk_cache_store(&disk, &(disk_chunk_t) { .nr = current_chunk_nr, .data = buffer });
    disk_add_to_list(&disk);
  }
}
