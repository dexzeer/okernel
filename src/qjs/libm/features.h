#ifndef QJS_FEATURES_H
#define QJS_FEATURES_H
#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
