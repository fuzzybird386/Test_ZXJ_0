struct adc_dt_spec { int unused; };
struct adc_sequence { void *buffer; unsigned buffer_size; };
#define ADC_DT_SPEC_GET(node) {0}
static inline bool adc_is_ready_dt(const struct adc_dt_spec *p) { (void)p;return true; }
static inline int adc_channel_setup_dt(const struct adc_dt_spec *p) { (void)p;return 0; }
static inline int adc_sequence_init_dt(const struct adc_dt_spec *p,struct adc_sequence *s) { (void)p;(void)s;return 0; }
static inline int adc_read_dt(const struct adc_dt_spec *p,struct adc_sequence *s) { (void)p;*(int16_t *)s->buffer=2000;return 0; }
static inline int adc_raw_to_millivolts_dt(const struct adc_dt_spec *p,int32_t *v) { (void)p;(void)v;return 0; }
