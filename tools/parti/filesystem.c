#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <iconv.h>
#include <getopt.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <blkid/blkid.h>
#include <json-c/json.h>

#ifdef __WITH_MEDIA_CHECK__
#include <mediacheck.h>
#endif

#include "disk.h"
#include "filesystem.h"
#include "util.h"

typedef struct {
  char *type;
  char *label;
  char *uuid;
} fs_detail_t;

typedef struct file_start_s {
  struct file_start_s *next;
  unsigned block;
  unsigned len;
  char *name;
} file_start_t;

int fs_probe(fs_detail_t *fs, disk_t *disk, uint64_t offset);
int fs_detail_fat(disk_t *disk, int indent, uint64_t sector);
int fs_detail_iso9660(json_object *json_fs, disk_t *disk, int indent, uint64_t sector);
void read_iso_detail(disk_t *disk);
void read_isoinfo(disk_t *disk);
void read_xorriso(disk_t *disk);

file_start_t *iso_offsets = NULL;
int iso_read = 0;

int fs_probe(fs_detail_t *fs, disk_t *disk, uint64_t offset)
{
  const char *data;

  *fs = (fs_detail_t) {};

  uint8_t buf[disk->block_size];

  for(uint64_t u = 0; u < 68 * 1024; u += disk->block_size) {
    disk_read(disk, buf, (offset + u) / disk->block_size, 1);
  }

  int disk_fd = disk_to_fd(disk, offset);

  if(disk_fd == -1) return 0;

  blkid_probe pr = blkid_new_probe();

  blkid_probe_set_device(pr, disk_fd, 0, 0);

  // blkid_probe_get_value(pr, n, &name, &data, &size)

  if(blkid_do_safeprobe(pr) == 0) {
    if(!blkid_probe_lookup_value(pr, "TYPE", &data, NULL)) {
      fs->type = strdup(data);

      if(!blkid_probe_lookup_value(pr, "LABEL", &data, NULL)) {
        fs->label = strdup(data);
      }

      if(!blkid_probe_lookup_value(pr, "UUID", &data, NULL)) {
        fs->uuid = strdup(data);
      }
    }
  }

  blkid_free_probe(pr);

  close(disk_fd);

  // if(fs->type) printf("ofs = %llu, type = '%s', label = '%s', uuid = '%s'\n", (unsigned long long) offset, fs->type, fs->label ?: "", fs->uuid ?: "");

  return fs->type ? 1 : 0;
}


/*
 * Print fat file system details.
 *
 * The fs starts at sector (sector size is disk->block_size).
 * The output is indented by 'indent' spaces.
 * If indent is 0, prints also a separator line.
 */
int fs_detail_fat(disk_t *disk, int indent, uint64_t sector)
{
  unsigned char buf[disk->block_size];
  int i;
  unsigned bpb_len, fat_bits, bpb32;
  unsigned bytes_p_sec, sec_p_cluster, resvd_sec, fats, root_ents, sectors;
  unsigned hidden, fat_secs, data_start, clusters, root_secs;
  unsigned drv_num;

  if(disk->block_size < 0x200) return 0;

  i = disk_read(disk, buf, sector, 1);

  if(i || read_word_le(buf + 0x1fe) != 0xaa55) return 0;

  if(read_byte(buf) == 0xeb) {
    i = 2 + (int8_t) read_byte(buf + 1);
  }
  else if(read_byte(buf) == 0xe9) {
    i = 3 + (int16_t) read_word_le(buf + 1);
  }
  else {
    i = 0;
  }

  if(i < 3) return 0;

  bpb_len = i;

  if(!strcmp(cname(buf + 3, 8), "NTFS")) return 0;

  bytes_p_sec = read_word_le(buf + 11);
  sec_p_cluster = read_byte(buf + 13);
  resvd_sec = read_word_le(buf + 14);
  fats = read_byte(buf + 16);
  root_ents = read_word_le(buf + 17);
  sectors = read_word_le(buf + 19);
  hidden = read_dword_le(buf + 28);
  if(!sectors) sectors = read_dword_le(buf + 32);
  fat_secs = read_word_le(buf + 22);
  bpb32 = fat_secs ? 0 : 1;
  if(bpb32) fat_secs = read_dword_le(buf + 36);

  if(!sec_p_cluster || !fats) return 0;

  // bytes_p_sec should be a power of 2 and > 0
  if(!bytes_p_sec || (bytes_p_sec & (bytes_p_sec - 1))) return 0;

  root_secs = (root_ents * 32 + bytes_p_sec - 1 ) / bytes_p_sec;

  data_start = resvd_sec + fats * fat_secs + root_secs;

  clusters = (sectors - data_start) / sec_p_cluster;

  fat_bits = 12;
  if(clusters >= 4085) fat_bits = 16;
  if(clusters >= 65525) fat_bits = 32;

  drv_num = read_byte(buf + (bpb32 ? 64 : 36));

  if(indent == 0) log_info(SEP "\n");

  log_info("%*sfat%u:\n", indent, "", fat_bits);

  indent += 2;

  log_info("%*ssector size: %u\n", indent, "", bytes_p_sec);

  log_info(
    "%*sbpb[%u], oem \"%s\", media 0x%02x, drive 0x%02x, hs %u/%u\n", indent, "",
    bpb_len,
    cname(buf + 3, 8),
    read_byte(buf + 21),
    drv_num,
    read_word_le(buf + 26),
    read_word_le(buf + 24)
  );

  if(read_byte(buf + (bpb32 ? 66 : 38)) == 0x29) {
    log_info("%*svol id 0x%08x, label \"%s\"", indent, "",
      read_dword_le(buf + (bpb32 ? 67 : 39)),
      cname(buf + (bpb32 ? 71 : 43), 11)
    );
    log_info(", fs type \"%s\"\n", cname(buf + (bpb32 ? 82 : 54), 8));
  }

  if(bpb32) {
    log_info("%*sextflags 0x%02x, fs ver %u.%u, fs info %u, backup bpb %u\n", indent, "",
      read_byte(buf + 40),
      read_byte(buf + 43), read_byte(buf + 42),
      read_word_le(buf + 48),
      read_word_le(buf + 50)
    );
  }

  log_info("%*sfs size %u, hidden %u, data start %u\n", indent, "",
    sectors,
    hidden,
    data_start
  );

  log_info("%*scluster size %u, clusters %u\n", indent, "",
    sec_p_cluster,
    clusters
  );

  log_info("%*sfats %u, fat size %u, fat start %u\n", indent, "",
    fats,
    fat_secs,
    resvd_sec
  );

  if(bpb32) {
    log_info("%*sroot cluster %u\n", indent, "",
      read_dword_le(buf + 44)
    );
  }
  else {
    log_info("%*sroot entries %u, root size %u, root start %u\n", indent, "",
      root_ents,
      root_secs,
      resvd_sec + fats * fat_secs
    );
  }

  return 1;
}


/*
 * Print iso9669 file system details.
 *
 * The fs starts at sector (sector size is disk->block_size).
 * The output is indented by 'indent' spaces.
 * If indent is 0, prints also a separator line.
 */
int fs_detail_iso9660(json_object *json_fs, disk_t *disk, int indent, uint64_t sector)
{
  if(sector || disk->block_size < 0x200) return 0;

#ifdef __WITH_MEDIA_CHECK__
  mediacheck_t *media = mediacheck_init(disk->name, 0);
  if(!media->err && media->signature.start) {
    uint64_t sig_block = media->signature.start;
    int sig_state = media->signature.state.id == sig_not_checked ? 1 : 0;

    unsigned sig_size = -1u;
    char *sig_file = iso_block_to_name(disk, sig_block, &sig_size);

    log_info("%*ssignature: %"PRIu64" (%ssigned)", indent, "",
      sig_block,
      sig_state ? "" : "not "
    );

    if(sig_file) log_info(", \"%s\"", sig_file);

    log_info("\n");

    json_object *json_sig = json_object_new_object();
    json_object_object_add(json_fs, "signature", json_sig);

    json_object_object_add(json_sig, "first_lba", json_object_new_int64(sig_block));
    if(sig_file) json_object_object_add(json_sig, "file_name", json_object_new_string(sig_file));
    if(sig_size != -1u) json_object_object_add(json_sig, "file_size", json_object_new_int(sig_size));
    json_object_object_add(json_sig, "signed", json_object_new_boolean(sig_state));
  }

  mediacheck_done(media);
#endif

  return 1;
}


/*
 * Print file system details.
 *
 * The fs starts at sector (sector size is disk->block_size).
 * The output is indented by 'indent' spaces.
 * If indent is 0, prints also a separator line.
 */
int dump_fs(disk_t *disk, int indent, uint64_t sector)
{
  char *s;
  fs_detail_t fs_detail;
  int fs_ok = fs_probe(&fs_detail, disk, sector * disk->block_size);

  if(!fs_ok) return fs_ok;

  if(indent == 0) {
    log_info(SEP "\nfile system:\n");
    indent += 2;
  }

  json_object *json_fs = json_object_new_object();
  json_object_object_add(disk->json_current, "filesystem", json_fs);

  json_object_object_add(json_fs, "block_size", json_object_new_int(disk->block_size));
  json_object_object_add(json_fs, "first_lba", json_object_new_int64(sector));

  json_object_object_add(json_fs, "type", json_object_new_string(fs_detail.type));
  log_info("%*sfs \"%s\"", indent, "", fs_detail.type);

  if(fs_detail.label) {
    json_object_object_add(json_fs, "label", json_object_new_string(fs_detail.label));
    log_info(", label \"%s\"", fs_detail.label);
  }

  if(fs_detail.uuid) {
    json_object_object_add(json_fs, "uuid", json_object_new_string(fs_detail.uuid));
    log_info(", uuid \"%s\"", fs_detail.uuid);
  }

  if((s = iso_block_to_name(disk, (sector * disk->block_size) >> 9, NULL))) {
    json_object_object_add(json_fs, "file_name", json_object_new_string(s));
    log_info(", \"%s\"", s);
  }
  log_info("\n");

  fs_detail_fat(disk, indent, sector);
  if(!strcmp(fs_detail.type, "iso9660")) fs_detail_iso9660(json_fs, disk, indent, sector);

  return fs_ok;
}


/*
 * block is in 512 byte units
 */
char *iso_block_to_name(disk_t *disk, unsigned block, unsigned *len)
{
  static char *buf = NULL;
  file_start_t *fs;
  char *name = NULL;

  if(!iso_read) read_iso_detail(disk);

  for(fs = iso_offsets; fs; fs = fs->next) {
    if(block >= fs->block && block < fs->block + (((fs->len + 2047) >> 11) << 2)) break;
  }

  if(fs) {
    if(len) *len = fs->len;
    if(block == fs->block) {
      name = fs->name;
    }
    else {
      free(buf);
      asprintf(&buf, "%s<+%u>", fs->name, block - fs->block);
      name = buf;
    }
  }

  return name;
}


void read_iso_detail(disk_t *disk)
{
  if(opt.xorriso) {
    read_xorriso(disk);
  }
  else {
    read_isoinfo(disk);
  }
}


void read_isoinfo(disk_t *disk)
{
  FILE *p;
  char *cmd, *s, *t, *line = NULL, *dir = NULL;
  size_t line_len = 0;
  unsigned u1, u2;
  file_start_t *fs;
  fs_detail_t fs_detail;

  iso_read = 1;

  if(!fs_probe(&fs_detail, disk, 0)) return;

  int tmp_fd = open("/tmp", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);

  if(tmp_fd == -1) return;

  int disk_fd = disk->fd;

  if(disk_fd == -1) disk_fd = disk_to_fd(disk, 0);

  if(disk_fd == -1) return;

  asprintf(&cmd, "/usr/bin/strace -e lseek -o /proc/self/fd/%d /usr/bin/isoinfo -R -l -i /proc/self/fd/%d 2>/dev/null", tmp_fd, disk_fd);

  if((p = popen(cmd, "r"))) {
    while(getline(&line, &line_len, p) != -1) {
      char *line_start = line;

      // isoinfo from mkisofs produces different output than the one from genisoimage
      // remove the optional 1st column (bsc#1097814)
      while(isspace(*line_start) || isdigit(*line_start)) line_start++;

      if(sscanf(line_start, "Directory listing of %m[^\n]", &s) == 1) {
        free(dir);
        dir = s;
      }
      else if(sscanf(line_start, "%*s %*s %*s %*s %u %*[^[][ %u %*u ] %m[^\n]", &u2, &u1, &s) == 3) {
        if(*s) {
          t = s + strlen(s) - 1;
          while(t >= s && isspace(*t)) *t-- = 0;

          if(strcmp(s, ".") && strcmp(s, "..")) {
            fs = calloc(1, sizeof *fs);
            fs->next = iso_offsets;
            iso_offsets = fs;
            fs->block = u1 << 2;
            fs->len = u2;
            asprintf(&fs->name, "%s%s", dir, s);
            free(s);
          }
        }
      }
    }

    pclose(p);
  }

  free(cmd);
  free(dir);

  if(disk->fd == -1) close(disk_fd);

  FILE *f = fdopen(tmp_fd, "r+");

  // isoinfo reads 2 kiB blocks after each lseek

  unsigned current_block_size = disk->block_size;
  disk->block_size = 2048;
  unsigned char tmp_buffer[2048];

  while(getline(&line, &line_len, f) != -1) {
    char *s;
    if((s = strchr(line, '='))) {
      int64_t ofs = strtoll(s + 1, NULL, 10);
      disk_read(disk, tmp_buffer, ofs / 2048, 1);
    }
  }

  free(line);

  disk->block_size = current_block_size;
}


void read_xorriso(disk_t *disk)
{
  FILE *p;
  char *cmd, *line = NULL, *dir = NULL;
  size_t line_len = 0;
  unsigned u1, u2;
  file_start_t *fs;
  fs_detail_t fs_detail;

  iso_read = 1;

  if(!fs_probe(&fs_detail, disk, 0)) return;

  int tmp_fd = open("/tmp", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);

  if(tmp_fd == -1) return;

  int disk_fd = disk->fd;

  if(disk_fd == -1) disk_fd = disk_to_fd(disk, 0);

  if(disk_fd == -1) return;

  asprintf(&cmd, "/usr/bin/strace -e lseek -o /proc/self/fd/%d /usr/bin/xorriso -indev /proc/self/fd/%d -find / -exec report_lba 2>/dev/null", tmp_fd, disk_fd);

  if((p = popen(cmd, "r"))) {
    while(getline(&line, &line_len, p) != -1) {
      char *s, *line_start = line;

      if(sscanf(line_start, "File data lba: %*u , %u , %*u , %u , '%m[^\n]", &u1, &u2, &s) == 3) {
        fs = calloc(1, sizeof *fs);
        fs->next = iso_offsets;
        iso_offsets = fs;
        fs->block = u1 << 2;
        fs->len = u2;
        size_t s_len = strlen(s);
        if(s_len > 0) s[s_len - 1] = 0;
        fs->name = s;
      }
    }

    pclose(p);
  }

  free(cmd);
  free(dir);

  if(disk->fd == -1) close(disk_fd);

  FILE *f = fdopen(tmp_fd, "r+");

  // xorriso reads 64 kiB blocks after each lseek

  unsigned current_block_size = disk->block_size;
  disk->block_size = 2048;
  unsigned char tmp_buffer[2048 * 32];

  while(getline(&line, &line_len, f) != -1) {
    char *s;
    if((s = strchr(line, '='))) {
      int64_t ofs = strtoll(s + 1, NULL, 10);
      disk_read(disk, tmp_buffer, ofs / 2048, 32);
    }
  }

  free(line);

  disk->block_size = current_block_size;
}
