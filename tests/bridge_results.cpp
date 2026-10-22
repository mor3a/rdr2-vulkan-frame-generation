#include "../src/sl_result_policy.h"
static_assert(static_cast<int>(sl::Result::eWarnOutOfVRAM)==39);
static_assert(fgvk::AcceptedSLResult(sl::Result::eOk));
static_assert(fgvk::AcceptedSLResult(sl::Result::eWarnOutOfVRAM));
static_assert(!fgvk::AcceptedSLResult(sl::Result::eErrorInvalidParameter));
static_assert(!fgvk::AcceptedSLResult(sl::Result::eErrorInvalidIntegration));
static_assert(static_cast<int>(sl::Result::eErrorOSDisabledHWS)==4);
static_assert(!fgvk::AcceptedSLResult(sl::Result::eErrorOSDisabledHWS));
