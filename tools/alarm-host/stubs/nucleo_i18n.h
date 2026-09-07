#pragma once
const char *nucleo_tr(const char *it, const char *en);
#ifndef TR
#define TR(it_, en_) nucleo_tr((it_), (en_))
#endif
