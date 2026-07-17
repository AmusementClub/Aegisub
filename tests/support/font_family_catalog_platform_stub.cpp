#include "../../src/font_family_catalog.h"

#ifdef _WIN32
// The catalog/cache unit tests inject deterministic builders and do not need
// to enumerate the host's installed fonts.
FontFamilyCatalog BuildFontFamilyCatalog() {
	return {};
}
#endif
