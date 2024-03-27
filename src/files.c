#include "files.h"

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

servefile_fn file_root;
servefile_fn file_cxadc0;
servefile_fn file_cxadc1;
servefile_fn file_linear;
servefile_fn file_start;
servefile_fn file_stop;
servefile_fn file_stats;

struct served_file SERVED_FILES[] = {
  {"/", "Content-Type: text/html; charset=utf-8\r\n", file_root},
  {"/cxadc0", "Content-Disposition: attachment; filename=\"cxadc0.u8\"\r\n", file_cxadc0},
  {"/cxadc1", "Content-Disposition: attachment; filename=\"cxadc1.u8\"\r\n", file_cxadc1},
  {"/linear", "Content-Disposition: attachment; filename=\"linear.raw\"\r\n", file_linear},
  {"/start", "Content-Type: text/json; charset=utf-8\r\n", file_start},
  {"/stop", "Content-Type: text/json; charset=utf-8\r\n", file_stop},
  {"/stats", "Content-Type: text/json; charset=utf-8\r\n", file_stats},
  {NULL}
};

struct atomic_ringbuffer {
  uint8_t* buf;
  size_t buf_size;
  _Atomic size_t written;
  _Atomic size_t read;
};

bool atomic_ringbuffer_init(struct atomic_ringbuffer* ctx, size_t buf_size) {
  static const int FLAGS = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

  void* buf = MAP_FAILED;

#ifdef MAP_HUGE_SHIFT
  static const size_t ONE_GB = (1u << 30);
  static const size_t TWO_MB = (2u << 20);
  if (buf_size % ONE_GB == 0 && buf_size > ONE_GB)
    buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, FLAGS | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT), -1, 0);
  if (MAP_FAILED == buf && buf_size % TWO_MB == 0 && buf_size > TWO_MB)
    buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, FLAGS | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT), -1, 0);
#endif
  if (MAP_FAILED == buf)
    buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, FLAGS, -1, 0);

  if (MAP_FAILED == buf) {
    perror("ringbuffer allocation failed");
    return false;
  }

  volatile uint8_t test = *(volatile uint8_t*)buf;
  (void)test;

  ctx->buf_size = buf_size;
  ctx->read = 0;
  ctx->written = 0;
  ctx->buf = (uint8_t*)buf;
  return true;
}

void atomic_ringbuffer_free(struct atomic_ringbuffer* ctx) {
  munmap(ctx->buf, ctx->buf_size);
  ctx->buf = NULL;
}

uint8_t* atomic_ringbuffer_get_write_ptr(struct atomic_ringbuffer* ctx) {
  return ctx->buf + (ctx->written % ctx->buf_size);
}

size_t atomic_ringbuffer_get_write_size(struct atomic_ringbuffer* ctx) {
  size_t buf_size = ctx->buf_size;
  size_t written = ctx->written;
  size_t read = ctx->read;
  size_t till_end = buf_size - (written % buf_size);
  size_t till_read = read + buf_size - written;
  return till_end < till_read ? till_end : till_read;
}

void atomic_ringbuffer_advance_written(struct atomic_ringbuffer* ctx, size_t count) {
  ctx->written += count;
}

uint8_t* atomic_ringbuffer_get_read_ptr(struct atomic_ringbuffer* ctx) {
  return ctx->buf + (ctx->read % ctx->buf_size);
}

size_t atomic_ringbuffer_get_read_size(struct atomic_ringbuffer* ctx) {
  size_t buf_size = ctx->buf_size;
  size_t written = ctx->written;
  size_t read = ctx->read;
  size_t till_end = buf_size - (read % buf_size);
  size_t till_written = written - read;
  return till_end < till_written ? till_end : till_written;
}

void atomic_ringbuffer_advance_read(struct atomic_ringbuffer* ctx, size_t count) {
  ctx->read += count;
}

enum capture_state {
  State_Idle = 0,
  State_Starting,
  State_Running,
  State_Stopping,

  State_Failed
};

const char* capture_state_to_str(enum capture_state state) {
  const char* NAMES[] = {"Idle", "Starting", "Running", "Stopping", "Failed"};
  return NAMES[(int)state];
}

struct {
  _Atomic enum capture_state cap_state;

  struct {
    pthread_t writer_thread;
    struct atomic_ringbuffer ring_buffer;

    // This is special and not protected by cap_state
    _Atomic pthread_t reader_thread;
  } cxadc0, cxadc1, linear;

  int cxadc0_fd;
  int cxadc1_fd;
  snd_pcm_t* linear_pcm_handle;
} g_state;

void* cxadc0_writer_thread(void*);
void* cxadc1_writer_thread(void*);
void* linear_writer_thread(void*);

static ssize_t timespec_to_nanos(const struct timespec* ts) {
  return (ssize_t)ts->tv_nsec + (ssize_t)ts->tv_sec * 1000000000;
}

void file_start(int fd) {
  static const int MODE = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;
  static const unsigned int RATE = 78125;
  static const unsigned int CHANNELS = 3;

  enum capture_state expected = State_Idle;
  if (!atomic_compare_exchange_strong(&g_state.cap_state, &expected, State_Starting)) {
    dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(expected));
    return;
  }

  if (!atomic_ringbuffer_init(&g_state.cxadc0.ring_buffer, 1 << 30)) {
    goto error;
  }
  if (!atomic_ringbuffer_init(&g_state.cxadc1.ring_buffer, 1 << 30)) {
    goto error;
  }
  if (!atomic_ringbuffer_init(&g_state.linear.ring_buffer, 9 * 2 << 20)) {
    goto error;
  }

  int err;
  snd_pcm_t* handle;
  if ((err = snd_pcm_open(&handle, "hw:CARD=CXADCADCClockGe", SND_PCM_STREAM_CAPTURE, MODE)) < 0) {
    fprintf(stderr, "cannot open ALSA device: %s\n", snd_strerror(err));
    goto error;
  }

  snd_pcm_hw_params_t* hw_params = NULL;
  snd_pcm_hw_params_alloca(&hw_params);

  if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
    fprintf(stderr, "cannot initialize hardware parameter structure: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    fprintf(stderr, "cannot set access type: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S24_3LE)) < 0) {
    fprintf(stderr, "cannot set sample format: %s\n", snd_strerror(err));
    goto error;
  }

  unsigned int rate = RATE;
  if ((err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0)) < 0) {
    fprintf(stderr, "cannot set sample rate: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, CHANNELS)) < 0) {
    fprintf(stderr, "cannot set channel count: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_hw_params(handle, hw_params)) < 0) {
    fprintf(stderr, "cannot set hw parameters: %s\n", snd_strerror(err));
    goto error;
  }

  snd_pcm_sw_params_t* sw_params = NULL;
  snd_pcm_sw_params_alloca(&sw_params);

  if ((err = snd_pcm_sw_params_current(handle, sw_params)) < 0) {
    fprintf(stderr, "cannot query sw parameters: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_sw_params_set_tstamp_mode(handle, sw_params, SND_PCM_TSTAMP_ENABLE)) < 0) {
    fprintf(stderr, "cannot set tstamp mode: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_sw_params_set_tstamp_type(handle, sw_params, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW)) < 0) {
    fprintf(stderr, "cannot set tstamp type: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_sw_params(handle, sw_params)) < 0) {
    fprintf(stderr, "cannot set hw parameters: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_prepare(handle)) < 0) {
    fprintf(stderr, "cannot prepare audio interface for use: %s\n", snd_strerror(err));
    goto error;
  }

  usleep(200000);

  struct timespec time1;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time1);

  snd_pcm_start(handle);

  const int cxadc0_fd = open("/dev/cxadc0", O_NONBLOCK);
  if (cxadc0_fd < 0) {
    perror("cannot open cxadc0");
    goto error;
  }

  const int cxadc1_fd = open("/dev/cxadc1", O_NONBLOCK);
  if (cxadc1_fd < 0) {
    perror("cannot open cxadc1");
    goto error;
  }

  struct timespec time2;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time2);

  const long ns = timespec_to_nanos(&time2) - timespec_to_nanos(&time1);
  printf("starting capture took %ldns\n", ns);

  g_state.linear_pcm_handle = handle;
  g_state.cxadc0_fd = cxadc0_fd;
  g_state.cxadc1_fd = cxadc1_fd;

  pthread_t thread_id;
  if ((err = pthread_create(&thread_id, NULL, cxadc0_writer_thread, NULL) != 0)) {
    fprintf(stderr, "can't create cxadc0 writer thread: %d\n", err);
    goto error;
  }
  g_state.cxadc0.writer_thread = thread_id;

  if ((err = pthread_create(&thread_id, NULL, cxadc1_writer_thread, NULL) != 0)) {
    fprintf(stderr, "can't create cxadc1 writer thread: %d\n", err);
    goto error;
  }
  g_state.cxadc1.writer_thread = thread_id;

  if ((err = pthread_create(&thread_id, NULL, linear_writer_thread, NULL) != 0)) {
    fprintf(stderr, "can't create linear writer thread: %d\n", err);
    goto error;
  }
  g_state.linear.writer_thread = thread_id;

  g_state.cap_state = State_Running;
  dprintf(fd, "{\"state\": \"%s\", \"time_ns\": %ld}", capture_state_to_str(State_Running), ns);
  return;

error:
  g_state.cap_state = State_Failed;
  dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(State_Failed));
}

void* cxadc_writer_thread(struct atomic_ringbuffer* buf, int fd) {
  while (g_state.cap_state != State_Stopping) {
    void* ptr = atomic_ringbuffer_get_write_ptr(buf);
    size_t len = atomic_ringbuffer_get_write_size(buf);
    if (len == 0) {
      fprintf(stderr, "ringbuffer full, may be dropping samples!!! THIS IS BAD!\n");
      usleep(1000);
      continue;
    }
    ssize_t count = read(fd, ptr, len);
    if (count == 0) {
      usleep(1);
      continue;
    }
    if (count < 0) {
      perror("read failed");
      break;
    }

    atomic_ringbuffer_advance_written(buf, count);
  }
  close(fd);
  return NULL;
}

void* linear_writer_thread(void* arg) {
  (void)arg;
  struct atomic_ringbuffer* buf = &g_state.linear.ring_buffer;
  snd_pcm_t* handle = g_state.linear_pcm_handle;

  while (g_state.cap_state != State_Stopping) {
    void* ptr = atomic_ringbuffer_get_write_ptr(buf);
    size_t len = atomic_ringbuffer_get_write_size(buf);
    if (len == 0) {
      fprintf(stderr, "ringbuffer full, may be dropping samples!!! THIS IS BAD!\n");
      usleep(1000);
      continue;
    }
    int count = snd_pcm_readi(handle, ptr, len / 9);
    if (count == 0 || count == -EAGAIN) {
      usleep(1);
      continue;
    }
    if (count < 0) {
      fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror(-count));
      break;
    }

    atomic_ringbuffer_advance_written(buf, count * 9);
  }
  snd_pcm_drop(handle);
  snd_pcm_close(handle);
  return NULL;
}

void* cxadc0_writer_thread(void* arg) {
  (void)arg;
  return cxadc_writer_thread(&g_state.cxadc0.ring_buffer, g_state.cxadc0_fd);
}

void* cxadc1_writer_thread(void* arg) {
  (void)arg;
  return cxadc_writer_thread(&g_state.cxadc1.ring_buffer, g_state.cxadc1_fd);
}

void file_stop(int fd) {
  enum capture_state expected = State_Running;
  if (!atomic_compare_exchange_strong(&g_state.cap_state, &expected, State_Stopping)) {
    dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(expected));
    return;
  }

  pthread_join(g_state.cxadc0.writer_thread, NULL);
  pthread_join(g_state.cxadc1.writer_thread, NULL);
  pthread_join(g_state.linear.writer_thread, NULL);
  while (g_state.cxadc0.reader_thread || g_state.cxadc1.reader_thread || g_state.linear.reader_thread)
    usleep(100000);

  atomic_ringbuffer_free(&g_state.cxadc0.ring_buffer);
  g_state.cxadc0.writer_thread = 0;
  g_state.cxadc0.reader_thread = 0;
  atomic_ringbuffer_free(&g_state.cxadc1.ring_buffer);
  g_state.cxadc1.writer_thread = 0;
  g_state.cxadc1.reader_thread = 0;
  atomic_ringbuffer_free(&g_state.linear.ring_buffer);
  g_state.linear.writer_thread = 0;
  g_state.linear.reader_thread = 0;

  g_state.cap_state = State_Idle;

  dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(State_Idle));
}

void file_root(int fd) {
  dprintf(fd, "Hello World!");
}

void pump_ringbuffer_to_fd(int fd, struct atomic_ringbuffer* buf, _Atomic pthread_t* pt) {
  pthread_t expected = 0;
  if (!atomic_compare_exchange_strong(pt, &expected, pthread_self())) {
    return;
  }
  while (g_state.cap_state != State_Running && g_state.cap_state != State_Stopping)
    usleep(1);

  while (g_state.cap_state == State_Running || g_state.cap_state == State_Stopping) {
    void* ptr = atomic_ringbuffer_get_read_ptr(buf);
    size_t len = atomic_ringbuffer_get_read_size(buf);
    if (len == 0) {
      if (g_state.cap_state == State_Stopping)
        break;
      usleep(1);
      continue;
    }
    ssize_t count = write(fd, ptr, len);
    if (count == 0) {
      usleep(1);
      continue;
    }
    if (count < 0) {
      perror("write failed");
      return;
    }

    atomic_ringbuffer_advance_read(buf, count);
  }

  *pt = 0;
}

void file_cxadc0(int fd) {
  pump_ringbuffer_to_fd(fd, &g_state.cxadc0.ring_buffer, &g_state.cxadc0.reader_thread);
}

void file_cxadc1(int fd) {
  pump_ringbuffer_to_fd(fd, &g_state.cxadc1.ring_buffer, &g_state.cxadc1.reader_thread);
}

void file_linear(int fd) {
  pump_ringbuffer_to_fd(fd, &g_state.linear.ring_buffer, &g_state.linear.reader_thread);
}

void file_stats(int fd) {
  (void)fd;
}
