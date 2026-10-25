#ifndef UI_OZONE_PLATFORM_WEBOS_NATIVE_GEOMETRY_H_
#define UI_OZONE_PLATFORM_WEBOS_NATIVE_GEOMETRY_H_

#include "base/unguessable_token.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace ui {
bool WebOSNativeMediaEnabled();
void UpdateWebOSNativeVideoGeometry(const base::UnguessableToken& token,
                                   const gfx::Rect& bounds,
                                   const gfx::Size& viewport);
}  // namespace ui
#endif
