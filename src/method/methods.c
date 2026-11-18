#include "inc/methods.h"
#include "inc/ec_method.h"
extern const ec_method_t method_cv, method_dpv;
int methods_register_all(void) {
 int rc = ec_method_register(&method_cv);
 return rc ? rc : ec_method_register(&method_dpv);
}
