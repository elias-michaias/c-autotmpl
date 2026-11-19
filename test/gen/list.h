#ifndef GEN_LIST_H
#define GEN_LIST_H

/* c-autotmpl: List(int) */
#ifndef C_AUTOTMPL_LIST_INT_8EB22343
#define C_AUTOTMPL_LIST_INT_8EB22343
#define T int
#include "../tmpl/list.h"
#undef T
#endif

/* c-autotmpl: Box(int) */
#ifndef C_AUTOTMPL_BOX_INT_20F046D8
#define C_AUTOTMPL_BOX_INT_20F046D8
#define T int
#include "../tmpl/box.h"
#undef T
#endif

/* c-autotmpl: List(Box_int) */
#ifndef C_AUTOTMPL_LIST_BOX_INT_CF66BC49
#ifdef GEN_BOX_H
#define C_AUTOTMPL_LIST_BOX_INT_CF66BC49
#define T Box_int
#include "../tmpl/list.h"
#undef T
#endif
#endif

/* c-autotmpl: Box(List_int) */
#ifndef C_AUTOTMPL_BOX_LIST_INT_B45E6A11
#define C_AUTOTMPL_BOX_LIST_INT_B45E6A11
#define T List_int
#include "../tmpl/box.h"
#undef T
#endif

/* c-autotmpl: List(Box_List_int) */
#ifndef C_AUTOTMPL_LIST_BOX_LIST_INT_398AD026
#ifdef GEN_BOX_H
#define C_AUTOTMPL_LIST_BOX_LIST_INT_398AD026
#define T Box_List_int
#include "../tmpl/list.h"
#undef T
#endif
#endif

#endif
