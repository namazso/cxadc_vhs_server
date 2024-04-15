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
servefile_fn file_cxadc;
servefile_fn file_linear;
servefile_fn file_start;
servefile_fn file_stop;
servefile_fn file_stats;

struct served_file SERVED_FILES[] = {
  {"/", "Content-Type: text/html; charset=utf-8\r\n", file_root},
  {"/cxadc", "Content-Disposition: attachment\r\n", file_cxadc},
  {"/linear", "Content-Disposition: attachment\r\n", file_linear},
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

struct cxadc_state {
  int fd;
  pthread_t writer_thread;
  struct atomic_ringbuffer ring_buffer;

  // This is special and not protected by cap_state
  _Atomic pthread_t reader_thread;
};

struct {
  _Atomic enum capture_state cap_state;
  struct cxadc_state cxadc[256];
  size_t cxadc_count;
  _Atomic size_t overflow_counter;

  struct {
    snd_pcm_t* handle;
    pthread_t writer_thread;
    struct atomic_ringbuffer ring_buffer;

    // This is special and not protected by cap_state
    _Atomic pthread_t reader_thread;
  } linear;

} g_state;

void* cxadc_writer_thread(void* id);
void* linear_writer_thread(void*);

static ssize_t timespec_to_nanos(const struct timespec* ts) {
  return (ssize_t)ts->tv_nsec + (ssize_t)ts->tv_sec * 1000000000;
}

void file_start(int fd, int argc, char** argv) {
  static const int MODE = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;
  static const unsigned int RATE = 78125;
  static const unsigned int CHANNELS = 3;

  enum capture_state expected = State_Idle;
  if (!atomic_compare_exchange_strong(&g_state.cap_state, &expected, State_Starting)) {
    dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(expected));
    return;
  }

  unsigned cxadc_array[256];
  unsigned cxadc_count = 0;
  for (int i = 0; i < argc; ++i) {
    unsigned num;
    if (1 == sscanf(argv[i], "cxadc%u", &num)) {
      cxadc_array[cxadc_count++] = num;
      if (cxadc_count == sizeof(cxadc_array) / sizeof(*cxadc_array))
        break;
    }
  }

  g_state.overflow_counter = 0;

  for (size_t i = 0; i < cxadc_count; ++i) {
    if (!atomic_ringbuffer_init(&g_state.cxadc[i].ring_buffer, 1 << 30)) {
      goto error;
    }
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

  struct timespec time1;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time1);

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
    fprintf(stderr, "cannot set sw parameters: %s\n", snd_strerror(err));
    goto error;
  }

  if ((err = snd_pcm_prepare(handle)) < 0) {
    fprintf(stderr, "cannot prepare audio interface for use: %s\n", snd_strerror(err));
    goto error;
  }

  snd_pcm_start(handle);

  struct timespec time2;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time2);

  for (size_t i = 0; i < cxadc_count; ++i) {
    char cxadc_name[32];
    sprintf(cxadc_name, "/dev/cxadc%u", cxadc_array[i]);
    const int cxadc_fd = open(cxadc_name, O_NONBLOCK);
    if (cxadc_fd < 0) {
      perror("cannot open cxadc");
      goto error;
    }
    g_state.cxadc[i].fd = cxadc_fd;
  }

  g_state.cxadc_count = cxadc_count;

  struct timespec time3;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time3);

  const long linear_ns = timespec_to_nanos(&time2) - timespec_to_nanos(&time1);
  const long cxadc_ns = timespec_to_nanos(&time3) - timespec_to_nanos(&time2);
  printf("starting linear took %ldns, starting cxadc took %ldns\n", linear_ns, cxadc_ns);

  g_state.linear.handle = handle;

  for (size_t i = 0; i < cxadc_count; ++i) {
    pthread_t thread_id;
    if ((err = pthread_create(&thread_id, NULL, cxadc_writer_thread, (void*)i) != 0)) {
      fprintf(stderr, "can't create cxadc writer thread: %d\n", err);
      goto error;
    }
    g_state.cxadc[i].writer_thread = thread_id;
  }

  pthread_t thread_id;
  if ((err = pthread_create(&thread_id, NULL, linear_writer_thread, NULL) != 0)) {
    fprintf(stderr, "can't create linear writer thread: %d\n", err);
    goto error;
  }
  g_state.linear.writer_thread = thread_id;

  g_state.cap_state = State_Running;
  dprintf(fd, "{\"state\": \"%s\", \"linear_ns\": %ld, \"cxadc_ns\": %ld}", capture_state_to_str(State_Running), linear_ns, cxadc_ns);
  return;

error:
  g_state.cap_state = State_Failed;
  dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(State_Failed));
}

void* cxadc_writer_thread(void* id) {
  struct atomic_ringbuffer* buf = &g_state.cxadc[(size_t)id].ring_buffer;
  const int fd = g_state.cxadc[(size_t)id].fd;

  while (g_state.cap_state != State_Stopping) {
    void* ptr = atomic_ringbuffer_get_write_ptr(buf);
    size_t len = atomic_ringbuffer_get_write_size(buf);
    if (len == 0) {
      ++g_state.overflow_counter;
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
  snd_pcm_t* handle = g_state.linear.handle;

  while (g_state.cap_state != State_Stopping) {
    void* ptr = atomic_ringbuffer_get_write_ptr(buf);
    size_t len = atomic_ringbuffer_get_write_size(buf);
    size_t len_samples = len / 9;
    if (len_samples == 0) {
      ++g_state.overflow_counter;
      fprintf(stderr, "ringbuffer full, may be dropping samples!!! THIS IS BAD!\n");
      usleep(1000);
      continue;
    }
    int count = snd_pcm_readi(handle, ptr, len_samples);
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

void file_stop(int fd, int argc, char** argv) {
  (void)argc;
  (void)argv;
  enum capture_state expected = State_Running;
  if (!atomic_compare_exchange_strong(&g_state.cap_state, &expected, State_Stopping)) {
    dprintf(fd, "{\"state\": \"%s\"}", capture_state_to_str(expected));
    return;
  }

  for (size_t i = 0; i < g_state.cxadc_count; ++i)
    pthread_join(g_state.cxadc[i].writer_thread, NULL);

  pthread_join(g_state.linear.writer_thread, NULL);

  while (g_state.linear.reader_thread)
    usleep(100000);

  for (size_t i = 0; i < g_state.cxadc_count; ++i) {
    while (g_state.cxadc[i].reader_thread)
      usleep(100000);
    atomic_ringbuffer_free(&g_state.cxadc[i].ring_buffer);
    g_state.cxadc[i].writer_thread = 0;
    g_state.cxadc[i].reader_thread = 0;
  }

  atomic_ringbuffer_free(&g_state.linear.ring_buffer);
  g_state.linear.writer_thread = 0;
  g_state.linear.reader_thread = 0;

  g_state.cap_state = State_Idle;

  dprintf(fd, "{\"state\": \"%s\", \"overflows\": %ld}", capture_state_to_str(State_Idle), g_state.overflow_counter);
}

void file_root(int fd, int argc, char** argv) {
  (void)argc;
  (void)argv;
  dprintf(fd, "Hello World!\n");
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

void file_cxadc(int fd, int argc, char** argv) {
  if (argc != 1)
    return;
  unsigned id;
  if (1 != sscanf(argv[0], "%u", &id) || id >= 256)
    return;
  pump_ringbuffer_to_fd(fd, &g_state.cxadc[id].ring_buffer, &g_state.cxadc[id].reader_thread);
}

void file_linear(int fd, int argc, char** argv) {
  (void)argc;
  (void)argv;
  pump_ringbuffer_to_fd(fd, &g_state.linear.ring_buffer, &g_state.linear.reader_thread);
}

void file_stats(int fd, int argc, char** argv) {
  (void)fd;
  (void)argc;
  (void)argv;
}
