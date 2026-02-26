#include "ducktape_engine_api.h"

int dtk_module_init(dtk_engine_t *engine, const dtk_engine_api_t *api)
{
  if (engine == NULL || api == NULL || api->bind_number == NULL) {
    return -1;
  }
  if (api->bind_number(engine, "MAGIC", 42.0) != 0) {
    return -1;
  }
  if (api->bind_number(engine, "ONE", 1.0) != 0) {
    return -1;
  }
  return 0;
}
