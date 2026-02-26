#include "ducktape_engine_api.h"

int dtk_module_init(dtk_engine_t *engine, const dtk_engine_api_t *api)
{
  if (engine == NULL || api == NULL || api->bind_number == NULL) {
    return -1;
  }
  if (api->bind_number(engine, "PI", 3.14159265358979323846) != 0) {
    return -1;
  }
  if (api->bind_number(engine, "E", 2.71828182845904523536) != 0) {
    return -1;
  }
  return 0;
}
