/*
 * libFuzzer harnesses for nnstreamer's GStreamer elements. One source, one target per -DFUZZ_<NAME>:
 * each input builds a short pipeline (appsrc ! <element> ! fakesink, or datareposrc ! fakesink),
 * delivers the fuzz bytes as stream buffers / element properties / files, and runs it to EOS or
 * ERROR. Elements come from the sanitized build tree (NNS_FUZZ_BUILD) through a private registry.
 *
 * State kept across inputs: the GStreamer registry and loaded plugins (read-only after init) and
 * the label file written once in LLVMFuzzerInitialize (never modified). Every pipeline is created
 * and destroyed per input; datareposrc's files are fresh mkstemp() files per input.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#ifndef NNS_FUZZ_BUILD
#define NNS_FUZZ_BUILD "/mayhem/mayhem/build-fuzz"
#endif

static char g_tmpdir[1024];
static char g_labels[1100];

static void quiet_log (const gchar *d, GLogLevelFlags l, const gchar *m, gpointer u)
{
  (void) d; (void) l; (void) m; (void) u;
}

/* A directory the target can write: $TMPDIR, /tmp, else the (writable) working directory. */
static void pick_tmpdir (void)
{
  const char *c[] = { getenv ("TMPDIR"), "/tmp", "." };
  for (size_t i = 0; i < G_N_ELEMENTS (c); i++) {
    if (c[i] && *c[i] && access (c[i], W_OK | X_OK) == 0) {
      char *r = realpath (c[i], NULL);
      snprintf (g_tmpdir, sizeof (g_tmpdir), "%s", r ? r : c[i]);
      free (r);
      return;
    }
  }
  snprintf (g_tmpdir, sizeof (g_tmpdir), ".");
}

#ifndef GST_SYS_PLUGIN_DIR
#define GST_SYS_PLUGIN_DIR "/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
#endif

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
  (void) argc; (void) argv;
  pick_tmpdir ();
  /*
   * No registry cache and no plugin-path scan: the plugins a pipeline needs are loaded explicitly
   * below, so start-up is the same (and fast) wherever the target runs.
   */
  setenv ("GST_REGISTRY_DISABLE", "yes", 1);
  setenv ("GST_REGISTRY_UPDATE", "no", 1);
  setenv ("GST_REGISTRY_FORK", "no", 1);
  setenv ("GST_PLUGIN_SYSTEM_PATH_1_0", "", 1);
  setenv ("GST_PLUGIN_SYSTEM_PATH", "", 1);
  setenv ("GST_PLUGIN_PATH_1_0", "", 1);
  setenv ("GST_PLUGIN_PATH", "", 1);
  /* nnstreamer's sub-plugin search paths (decoders, converters) come from its ini file; the
   * sub-plugins are also linked into the harness, so they are registered even without it. */
  char ini[1200];
  snprintf (ini, sizeof (ini), "%s/nnsfuzz-conf-%d.ini", g_tmpdir, (int) getpid ());
  FILE *cf = fopen (ini, "w");
  if (cf) {
    fprintf (cf, "[common]\nenable_envvar=False\nenable_symlink=False\n"
        "[decoder]\ndecoders=%s/ext/nnstreamer/tensor_decoder/\n"
        "[converter]\nconverters=%s/ext/nnstreamer/tensor_converter/\n",
        NNS_FUZZ_BUILD, NNS_FUZZ_BUILD);
    fclose (cf);
    setenv ("NNSTREAMER_CONF", ini, 1);
  }
  g_log_set_default_handler (quiet_log, NULL);
  gst_init (NULL, NULL);
  static const char *const plugins[] = {
    GST_SYS_PLUGIN_DIR "/libgstcoreelements.so",
    GST_SYS_PLUGIN_DIR "/libgstapp.so",
    NNS_FUZZ_BUILD "/gst/nnstreamer/libnnstreamer.so",
    NNS_FUZZ_BUILD "/gst/datarepo/libgstdatarepo.so",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (plugins); i++) {
    GError *e = NULL;
    GstPlugin *pl = gst_plugin_load_file (plugins[i], &e);
    if (!pl) {
      fprintf (stderr, "nnsfuzz: cannot load %s: %s\n", plugins[i], e ? e->message : "?");
      abort ();
    }
    gst_object_unref (pl);
  }

  snprintf (g_labels, sizeof (g_labels), "%s/nnsfuzz-labels-%d.txt", g_tmpdir, (int) getpid ());
  FILE *f = fopen (g_labels, "w");
  if (f) {
    for (int i = 0; i < 100; i++)
      fprintf (f, "label%d\n", i);
    fclose (f);
  }
  return 0;
}

/* Runs the pipeline until EOS or ERROR, then tears it down. */
static void run_pipeline (GstElement *pipe, const uint8_t *const *mems, const size_t *lens, int nmem)
{
  GstStateChangeReturn r = gst_element_set_state (pipe, GST_STATE_PLAYING);
  if (r != GST_STATE_CHANGE_FAILURE) {
    GstElement *src = gst_bin_get_by_name (GST_BIN (pipe), "src");
    if (src && GST_IS_APP_SRC (src)) {
      if (nmem > 0) {
        GstBuffer *buf = gst_buffer_new ();
        for (int i = 0; i < nmem; i++) {
          gpointer p = g_malloc (lens[i] ? lens[i] : 1);
          if (lens[i])
            memcpy (p, mems[i], lens[i]);
          gst_buffer_append_memory (buf, gst_memory_new_wrapped (0, p, lens[i], 0, lens[i], p, g_free));
        }
        GST_BUFFER_PTS (buf) = 0;
        gst_app_src_push_buffer (GST_APP_SRC (src), buf);
      }
      gst_app_src_end_of_stream (GST_APP_SRC (src));
    }
    if (src)
      gst_object_unref (src);
    GstBus *bus = gst_element_get_bus (pipe);
    /* bounded: a pipeline that never reaches EOS/ERROR must not hang the input */
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, 3 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg)
      gst_message_unref (msg);
    gst_object_unref (bus);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
}

static GstElement *launch (const char *desc)
{
  GError *err = NULL;
  GstElement *p = gst_parse_launch (desc, &err);
  if (err) {
    g_error_free (err);
    if (p)
      gst_object_unref (p);
    return NULL;
  }
  return p;
}

/* Sets the appsrc caps from a caps string; FALSE if GStreamer cannot parse it. */
static gboolean set_src_caps (GstElement *p, const char *str)
{
  GstCaps *c = gst_caps_from_string (str);
  if (!c)
    return FALSE;
  GstElement *src = gst_bin_get_by_name (GST_BIN (p), "src");
  g_object_set (src, "caps", c, NULL);
  gst_object_unref (src);
  gst_caps_unref (c);
  return TRUE;
}

/* Reads a NUL-terminated string from the input (at most to the end); advances the cursor. */
static char *take_str (const uint8_t **d, size_t *n)
{
  const uint8_t *z = memchr (*d, 0, *n);
  size_t l = z ? (size_t) (z - *d) : *n;
  char *s = g_strndup ((const char *) *d, l);
  *d += z ? l + 1 : l;
  *n -= z ? l + 1 : l;
  return s;
}

/* Splits the rest of the input into up to 4 memories: byte0 = count, then 2-byte lengths. */
static int split_mems (const uint8_t *d, size_t n, const uint8_t **mems, size_t *lens)
{
  if (n < 1)
    return 0;
  int k = (d[0] & 3) + 1;
  d++; n--;
  int i;
  for (i = 0; i < k - 1 && n >= 2; i++) {
    size_t l = (size_t) d[0] | ((size_t) d[1] << 8);
    d += 2; n -= 2;
    if (l > n)
      l = n;
    mems[i] = d; lens[i] = l;
    d += l; n -= l;
  }
  mems[i] = d; lens[i] = n;
  return i + 1;
}

static const char *const TYPES[] = { "int32", "uint32", "int16", "uint16", "int8", "uint8",
  "float64", "float32", "int64", "uint64" };
static const size_t TSIZE[] = { 4, 4, 2, 2, 1, 1, 8, 4, 8, 8 };

/*
 * Static tensors header: byte num (1..4), then per tensor a type byte and 4 dim bytes (1..64).
 * Builds the caps string and splits the remaining data into one memory per tensor (by the
 * expected tensor size; the last memory takes what is left, so a size mismatch reaches the
 * element's buffer validation).
 */
static int tensors_header (const uint8_t **d, size_t *n, GString *caps,
    const uint8_t **mems, size_t *lens)
{
  if (*n < 1)
    return -1;
  int num = ((*d)[0] & 3) + 1;
  (*d)++; (*n)--;
  if (*n < (size_t) num * 5)
    return -1;
  GString *dims = g_string_new (NULL), *types = g_string_new (NULL);
  size_t sizes[4];
  for (int i = 0; i < num; i++) {
    const uint8_t *h = *d + i * 5;
    int ti = h[0] % 10;
    size_t sz = TSIZE[ti];
    g_string_append_printf (types, "%s%s", i ? "," : "", TYPES[ti]);
    g_string_append (dims, i ? "," : "");
    for (int j = 0; j < 4; j++) {
      unsigned dim = (h[1 + j] & 0x3f) + 1;
      sz *= dim;
      g_string_append_printf (dims, "%s%u", j ? ":" : "", dim);
    }
    sizes[i] = sz;
  }
  *d += num * 5; *n -= num * 5;
  g_string_append_printf (caps, "other/tensors,format=static,num_tensors=%d,"
      "dimensions=(string)\"%s\",types=(string)\"%s\",framerate=(fraction)0/1",
      num, dims->str, types->str);
  g_string_free (dims, TRUE);
  g_string_free (types, TRUE);
  return num;
  (void) mems; (void) lens; (void) sizes;
}

static int split_by_tensors (const uint8_t *d, size_t n, int num, const GString *caps,
    const uint8_t **mems, size_t *lens)
{
  /* Recompute sizes from the caps' own dims would duplicate parsing; we pass sizes via caps text. */
  (void) caps;
  int i;
  for (i = 0; i < num - 1; i++) {
    size_t l = lens[i] < n ? lens[i] : n;
    mems[i] = d; lens[i] = l;
    d += l; n -= l;
  }
  mems[i] = d; lens[i] = n;
  return num;
}

#if defined(FUZZ_CONVERTER_FLEXIBLE) || defined(FUZZ_CONVERTER_FLATBUF) || \
    defined(FUZZ_CONVERTER_PROTOBUF) || defined(FUZZ_CONVERTER_FLEXBUF) || defined(FUZZ_SPARSE_DEC)
int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
#if defined(FUZZ_CONVERTER_FLEXIBLE)
  const char *desc = "appsrc name=src caps=other/tensors,format=flexible,framerate=0/1 ! "
      "tensor_converter ! fakesink sync=false";
#elif defined(FUZZ_CONVERTER_FLATBUF)
  const char *desc = "appsrc name=src caps=other/flatbuf-tensor,framerate=0/1 ! "
      "tensor_converter ! fakesink sync=false";
#elif defined(FUZZ_CONVERTER_PROTOBUF)
  const char *desc = "appsrc name=src caps=other/protobuf-tensor,framerate=0/1 ! "
      "tensor_converter ! fakesink sync=false";
#elif defined(FUZZ_CONVERTER_FLEXBUF)
  const char *desc = "appsrc name=src caps=other/flexbuf,framerate=0/1 ! "
      "tensor_converter ! fakesink sync=false";
#else
  const char *desc = "appsrc name=src caps=other/tensors,format=sparse,framerate=0/1 ! "
      "tensor_sparse_dec ! fakesink sync=false";
#endif
  const uint8_t *mems[4];
  size_t lens[4];
  int nmem;
#if defined(FUZZ_CONVERTER_FLEXIBLE) || defined(FUZZ_SPARSE_DEC)
  /* flexible/sparse tensors carry one header+data per memory */
  nmem = split_mems (data, size, mems, lens);
#else
  mems[0] = data; lens[0] = size; nmem = 1;
#endif
  if (nmem < 1)
    return -1;
  GstElement *p = launch (desc);
  if (!p)
    return 0;
  run_pipeline (p, mems, lens, nmem);
  return 0;
}
#endif

static size_t expected_sizes[4];

static int header_and_split (const uint8_t **d, size_t *n, GString *caps,
    const uint8_t **mems, size_t *lens)
{
  const uint8_t *hdr = *d;
  int num = tensors_header (d, n, caps, mems, lens);
  if (num < 1)
    return -1;
  for (int i = 0; i < num; i++) {
    const uint8_t *h = hdr + 1 + i * 5;
    size_t sz = TSIZE[h[0] % 10];
    for (int j = 0; j < 4; j++)
      sz *= (h[1 + j] & 0x3f) + 1;
    expected_sizes[i] = sz;
    lens[i] = sz;
  }
  return num;
}

#ifdef FUZZ_DECODER
struct dec_mode { const char *mode; const char *o1, *o2, *o3, *o4, *o5; };
#define LBL "@LABELS@"
static const struct dec_mode DEC[] = {
  { "direct_video", NULL, NULL, NULL, NULL, NULL },
  { "octet_stream", NULL, NULL, NULL, NULL, NULL },
  { "image_labeling", LBL, NULL, NULL, NULL, NULL },
  { "image_segment", "tflite-deeplab", NULL, NULL, NULL, NULL },
  { "image_segment", "snpe-deeplab", NULL, NULL, NULL, NULL },
  { "image_segment", "snpe-depth", NULL, NULL, NULL, NULL },
  { "pose_estimation", "64:64", "16:16", NULL, NULL, NULL },
  { "pose_estimation", "64:64", "16:16", LBL, "heatmap-offset", NULL },
  { "bounding_boxes", "yolov5", LBL, "0:0.25:0.45", "64:64", "64:64" },
  { "bounding_boxes", "yolov8", LBL, "0:0.25:0.45", "64:64", "64:64" },
  { "bounding_boxes", "mobilenet-ssd-postprocess", LBL, "0:1:2:3,50", "64:64", "64:64" },
  { "bounding_boxes", "mp-palm-detection", LBL, "0.5:4:1.0:1.0:0.5:0.5:8:16:16:16", "64:64", "64:64" },
  { "bounding_boxes", "ov-person-detection", NULL, NULL, "64:64", "64:64" },
  { "flatbuf", NULL, NULL, NULL, NULL, NULL },
  { "protobuf", NULL, NULL, NULL, NULL, NULL },
  { "flexbuf", NULL, NULL, NULL, NULL, NULL },
};

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (size < 2)
    return -1;
  const struct dec_mode *m = &DEC[data[0] % G_N_ELEMENTS (DEC)];
  data++; size--;
  /* option3 override: a fuzzed mode-option string (empty = keep the table's) */
  char *o3 = take_str (&data, &size);
  GString *caps = g_string_new (NULL);
  const uint8_t *mems[4];
  size_t lens[4];
  int num = header_and_split (&data, &size, caps, mems, lens);
  if (num < 1) {
    g_free (o3);
    g_string_free (caps, TRUE);
    return -1;
  }
  split_by_tensors (data, size, num, caps, mems, lens);
  /* Built by hand: tensor_decoder only has pad caps once its mode is set, so it must be
   * configured before linking (gst_parse_launch would fail to link it). */
  GstElement *p = gst_pipeline_new (NULL);
  GstElement *src = gst_element_factory_make ("appsrc", "src");
  GstElement *dec = gst_element_factory_make ("tensor_decoder", "dec");
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  GstCaps *c = gst_caps_from_string (caps->str);
  g_string_free (caps, TRUE);
  if (!src || !dec || !sink || !c) {
    if (c)
      gst_caps_unref (c);
    if (src) gst_object_unref (src);
    if (dec) gst_object_unref (dec);
    if (sink) gst_object_unref (sink);
    gst_object_unref (p);
    g_free (o3);
    return 0;
  }
  g_object_set (src, "caps", c, NULL);
  gst_caps_unref (c);
  g_object_set (sink, "sync", FALSE, NULL);
  gst_bin_add_many (GST_BIN (p), src, dec, sink, NULL);
  gst_object_ref (dec);
  const char *opts[5] = { m->o1, m->o2, *o3 ? o3 : m->o3, m->o4, m->o5 };
  g_object_set (dec, "mode", m->mode, NULL);
  for (int i = 0; i < 5; i++) {
    if (!opts[i])
      continue;
    char name[16];
    snprintf (name, sizeof (name), "option%d", i + 1);
    g_object_set (dec, name, strcmp (opts[i], LBL) ? opts[i] : g_labels, NULL);
  }
  if (!gst_element_link_many (src, dec, sink, NULL)) {
    gst_object_unref (dec);
    gst_object_unref (p);
    g_free (o3);
    return 0;
  }
  gst_object_unref (dec);
  run_pipeline (p, mems, lens, num);
  g_free (o3);
  return 0;
}
#endif

#ifdef FUZZ_TRANSFORM
static const char *const TMODES[] = { "dimchg", "typecast", "arithmetic", "transpose", "stand",
  "clamp", "padding" };

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (size < 2)
    return -1;
  const char *mode = TMODES[data[0] % G_N_ELEMENTS (TMODES)];
  data++; size--;
  char *option = take_str (&data, &size);
  char *apply = take_str (&data, &size);
  GString *caps = g_string_new (NULL);
  const uint8_t *mems[4];
  size_t lens[4];
  int num = header_and_split (&data, &size, caps, mems, lens);
  GstElement *p = NULL;
  if (num >= 1) {
    split_by_tensors (data, size, num, caps, mems, lens);
    p = launch ("appsrc name=src ! tensor_transform name=tr ! fakesink sync=false");
    if (p && !set_src_caps (p, caps->str)) {
      gst_object_unref (p);
      p = NULL;
    }
  }
  g_string_free (caps, TRUE);
  if (p) {
    GstElement *tr = gst_bin_get_by_name (GST_BIN (p), "tr");
    gst_util_set_object_arg (G_OBJECT (tr), "mode", mode);
    g_object_set (tr, "option", option, NULL);
    if (*apply)
      g_object_set (tr, "apply", apply, NULL);
    gst_object_unref (tr);
    run_pipeline (p, mems, lens, num);
  }
  g_free (option);
  g_free (apply);
  return num >= 1 ? 0 : -1;
}
#endif

#ifdef FUZZ_DATAREPOSRC
static int write_tmp (char *path, size_t cap, const char *tag, const uint8_t *d, size_t n)
{
  snprintf (path, cap, "%s/nnsfuzz-%s-XXXXXX", g_tmpdir, tag);
  int fd = mkstemp (path);
  if (fd < 0)
    return -1;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write (fd, d + off, n - off);
    if (w <= 0)
      break;
    off += (size_t) w;
  }
  close (fd);
  return 0;
}

/* Input: JSON metadata, NUL, sample data file. */
int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  const uint8_t *z = memchr (data, 0, size);
  size_t jl = z ? (size_t) (z - data) : size;
  char jpath[1200], dpath[1200];
  if (write_tmp (jpath, sizeof (jpath), "json", data, jl) < 0)
    return 0;
  const uint8_t *rest = z ? z + 1 : data + size;
  if (write_tmp (dpath, sizeof (dpath), "data", rest, (size_t) (data + size - rest)) < 0) {
    unlink (jpath);
    return 0;
  }
  GstElement *p = launch ("datareposrc name=src ! fakesink sync=false");
  if (p) {
    GstElement *src = gst_bin_get_by_name (GST_BIN (p), "src");
    g_object_set (src, "location", dpath, "json", jpath, "is-shuffle", FALSE, NULL);
    gst_object_unref (src);
    run_pipeline (p, NULL, NULL, 0);
  }
  unlink (jpath);
  unlink (dpath);
  return 0;
}
#endif

#ifdef FUZZ_FILTER
/*
 * tensor_filter property parsing (input/output-combination, layouts, dims/types, accelerator,
 * custom) and its invoke path, over a passthrough model registered through nnstreamer's public
 * custom-easy API (registered once at init; the model itself keeps no state).
 * Input: 8 NUL-terminated property strings, then a static tensors header + data.
 */
#include <tensor_filter_custom_easy.h>
#include <nnstreamer_plugin_api_util.h>

static int passthrough (void *pd, const GstTensorFilterProperties *prop,
    const GstTensorMemory *in, GstTensorMemory *out)
{
  (void) pd; (void) prop;
  size_t n = in[0].size < out[0].size ? in[0].size : out[0].size;
  memcpy (out[0].data, in[0].data, n);
  return 0;
}

static int g_registered;

static const char *const FPROPS[] = { "input-combination", "output-combination", "inputlayout",
  "outputlayout", "accelerator", "custom", "input", "inputtype" };

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (!g_registered) {
    GstTensorsInfo info;
    gst_tensors_info_init (&info);
    info.num_tensors = 1;
    info.info[0].type = _NNS_UINT8;
    info.info[0].dimension[0] = 3;
    info.info[0].dimension[1] = 4;
    info.info[0].dimension[2] = 4;
    info.info[0].dimension[3] = 1;
    NNS_custom_easy_register ("nnsfuzz", passthrough, NULL, &info, &info);
    g_registered = 1;
  }
  char *vals[G_N_ELEMENTS (FPROPS)];
  for (size_t i = 0; i < G_N_ELEMENTS (FPROPS); i++)
    vals[i] = take_str (&data, &size);
  GString *caps = g_string_new (NULL);
  const uint8_t *mems[4];
  size_t lens[4];
  int num = header_and_split (&data, &size, caps, mems, lens);
  GstElement *p = NULL;
  if (num >= 1) {
    split_by_tensors (data, size, num, caps, mems, lens);
    p = launch ("appsrc name=src ! tensor_filter name=f framework=custom-easy model=nnsfuzz ! "
        "fakesink sync=false");
    if (p && !set_src_caps (p, caps->str)) {
      gst_object_unref (p);
      p = NULL;
    }
  }
  g_string_free (caps, TRUE);
  if (p) {
    GstElement *f = gst_bin_get_by_name (GST_BIN (p), "f");
    for (size_t i = 0; i < G_N_ELEMENTS (FPROPS); i++)
      if (*vals[i])
        g_object_set (f, FPROPS[i], vals[i], NULL);
    gst_object_unref (f);
    run_pipeline (p, mems, lens, num);
  }
  for (size_t i = 0; i < G_N_ELEMENTS (FPROPS); i++)
    g_free (vals[i]);
  return num >= 1 ? 0 : -1;
}
#endif

#ifdef FUZZ_ELEMENTS
/*
 * Option-string parsing + data paths of tensor_if, tensor_split, tensor_demux and
 * tensor_aggregator. Input: selector byte, one NUL-terminated value per property of the chosen
 * element (set with gst_util_set_object_arg, so enum nicks and numbers parse as GStreamer does;
 * empty = default), then a static tensors header + data.
 */
struct elem { const char *name; const char *props[8]; };
static const struct elem ELEMS[] = {
  { "tensor_if", { "compared-value", "compared-value-option", "supplied-value", "operator",
      "then", "then-option", "else", "else-option" } },
  { "tensor_split", { "tensorseg", "tensorpick" } },
  { "tensor_demux", { "tensorpick" } },
  { "tensor_aggregator", { "frames-in", "frames-out", "frames-flush", "frames-dim", "concat" } },
};

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (size < 2)
    return -1;
  const struct elem *e = &ELEMS[data[0] % G_N_ELEMENTS (ELEMS)];
  data++; size--;
  char *vals[8] = { 0 };
  int np = 0;
  while (np < 8 && e->props[np]) {
    vals[np] = take_str (&data, &size);
    np++;
  }
  GString *caps = g_string_new (NULL);
  const uint8_t *mems[4];
  size_t lens[4];
  int num = header_and_split (&data, &size, caps, mems, lens);
  GstElement *p = NULL;
  if (num >= 1) {
    split_by_tensors (data, size, num, caps, mems, lens);
    gchar *desc = g_strdup_printf ("appsrc name=src ! %s name=e ! fakesink sync=false", e->name);
    p = launch (desc);
    g_free (desc);
    if (p && !set_src_caps (p, caps->str)) {
      gst_object_unref (p);
      p = NULL;
    }
  }
  g_string_free (caps, TRUE);
  if (p) {
    GstElement *el = gst_bin_get_by_name (GST_BIN (p), "e");
    for (int i = 0; i < np; i++)
      if (*vals[i])
        gst_util_set_object_arg (G_OBJECT (el), e->props[i], vals[i]);
    gst_object_unref (el);
    run_pipeline (p, mems, lens, num);
  }
  for (int i = 0; i < np; i++)
    g_free (vals[i]);
  return num >= 1 ? 0 : -1;
}
#endif

#ifdef FUZZ_CAPS
/*
 * nnstreamer's public caps/tensor-info string parsers (what every tensor_* element runs on
 * negotiated caps and on its dimension/type/name properties). Input: four NUL-terminated
 * strings: a caps string, then a dimensions, a types and a names string. Parsed configs are
 * validated and turned back into caps/strings, as an element does when it fixates.
 */
#include <nnstreamer_plugin_api.h>
#include <nnstreamer_plugin_api_util.h>

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  char *caps_s = take_str (&data, &size);
  char *dims = take_str (&data, &size);
  char *types = take_str (&data, &size);
  char *names = take_str (&data, &size);

  GstCaps *caps = gst_caps_from_string (caps_s);
  if (caps) {
    for (guint i = 0; i < gst_caps_get_size (caps); i++) {
      GstTensorsConfig cfg;
      gst_tensors_config_init (&cfg);
      if (gst_tensors_config_from_structure (&cfg, gst_caps_get_structure (caps, i)) &&
          gst_tensors_config_validate (&cfg)) {
        gchar *s = gst_tensors_config_to_string (&cfg);
        g_free (s);
        GstCaps *out = gst_tensors_caps_from_config (&cfg);
        if (out)
          gst_caps_unref (out);
      }
      gst_tensors_config_free (&cfg);
    }
    if (gst_caps_is_fixed (caps)) {
      GstTensorsConfig cfg;
      gst_tensors_config_init (&cfg);
      if (gst_tensors_config_from_caps (&cfg, caps, TRUE)) {
        GstCaps *out = gst_tensor_caps_from_config (&cfg);
        if (out)
          gst_caps_unref (out);
      }
      gst_tensors_config_free (&cfg);
    }
    gst_caps_unref (caps);
  }

  GstTensorsInfo info;
  gst_tensors_info_init (&info);
  guint nd = gst_tensors_info_parse_dimensions_string (&info, dims);
  guint nt = gst_tensors_info_parse_types_string (&info, types);
  gst_tensors_info_parse_names_string (&info, names);
  guint n = MAX (nd, nt);
  info.num_tensors = MIN (n, NNS_TENSOR_SIZE_LIMIT);
  gchar *ds = gst_tensors_info_get_dimensions_string (&info);
  gchar *ts = gst_tensors_info_get_types_string (&info);
  g_free (ds);
  g_free (ts);
  gst_tensors_info_free (&info);

  g_free (caps_s);
  g_free (dims);
  g_free (types);
  g_free (names);
  return 0;
}
#endif

#ifdef FUZZ_CONVERTER_RAW
/*
 * tensor_converter's raw-media chains (octet-stream, text, video, audio): the input-dim,
 * input-type and frames-per-tensor property strings decide how arbitrary stream bytes are cut
 * into tensors. Input: selector byte (caps kind / format), two parameter bytes (width/height or
 * channels/rate), three NUL-terminated property values (set with gst_util_set_object_arg;
 * empty = default), then the stream data as one buffer.
 */
static const char *const VFMT[] = { "GRAY8", "RGB", "BGR", "RGBx", "BGRx", "xRGB", "xBGR",
  "RGBA", "BGRA", "ARGB", "ABGR", "I420", "NV12", "NV21", "GRAY16_LE", "GRAY16_BE", "YUY2",
  "RGBP", "BGRP" };
static const char *const AFMT[] = { "S8", "U8", "S16LE", "U16LE", "S32LE", "U32LE", "F32LE",
  "F64LE" };
static const char *const CPROPS[] = { "input-dim", "input-type", "frames-per-tensor" };

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (size < 3)
    return -1;
  unsigned sel = data[0], a = data[1], b = data[2];
  data += 3; size -= 3;
  gchar *caps;
  switch (sel % 4) {
    case 0:
      caps = g_strdup ("application/octet-stream,framerate=(fraction)0/1");
      break;
    case 1:
      caps = g_strdup ("text/x-raw,format=(string)utf8,framerate=(fraction)0/1");
      break;
    case 2:
      caps = g_strdup_printf ("video/x-raw,format=(string)%s,width=(int)%u,height=(int)%u,"
          "framerate=(fraction)0/1", VFMT[(sel / 4) % G_N_ELEMENTS (VFMT)], (a & 0x3f) + 1,
          (b & 0x3f) + 1);
      break;
    default:
      caps = g_strdup_printf ("audio/x-raw,format=(string)%s,channels=(int)%u,rate=(int)%u,"
          "layout=(string)interleaved", AFMT[(sel / 4) % G_N_ELEMENTS (AFMT)], (a & 7) + 1,
          ((unsigned) b + 1) * 100);
      break;
  }
  char *vals[G_N_ELEMENTS (CPROPS)];
  for (size_t i = 0; i < G_N_ELEMENTS (CPROPS); i++)
    vals[i] = take_str (&data, &size);
  GstElement *p = launch ("appsrc name=src ! tensor_converter name=c ! fakesink sync=false");
  if (p && !set_src_caps (p, caps)) {
    gst_object_unref (p);
    p = NULL;
  }
  if (p) {
    GstElement *c = gst_bin_get_by_name (GST_BIN (p), "c");
    for (size_t i = 0; i < G_N_ELEMENTS (CPROPS); i++)
      if (*vals[i])
        gst_util_set_object_arg (G_OBJECT (c), CPROPS[i], vals[i]);
    gst_object_unref (c);
    const uint8_t *mems[1] = { data };
    size_t lens[1] = { size };
    run_pipeline (p, mems, lens, 1);
  }
  for (size_t i = 0; i < G_N_ELEMENTS (CPROPS); i++)
    g_free (vals[i]);
  g_free (caps);
  return 0;
}
#endif
