#pragma once
const char *nucleo_tr(const char *it, const char *en);
const char *nucleo_tr5(const char *it, const char *en, const char *es, const char *fr, const char *de);
#ifndef TR
#define TR(it_, en_) nucleo_tr((it_), (en_))
#endif
#ifndef TR5
#define TR5(it_, en_, es_, fr_, de_) nucleo_tr5((it_), (en_), (es_), (fr_), (de_))
#endif
