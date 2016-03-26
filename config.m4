dnl $Id$
dnl config.m4 for extension cfc

dnl Comments in this file start with the string 'dnl'.
dnl Remove where necessary. This file will not work
dnl without editing.

dnl If your extension references something external, use with:

PHP_ARG_WITH(cfc, for cfcsupport,
Make sure that the comment is aligned:
[  --with-cfc             Include fcf support])

dnl Otherwise use enable:

PHP_ARG_ENABLE(cfc, whether to enable cfc support,
Make sure that the comment is aligned:
[  --enable-cfc Enable cfc support])

if test "$PHP_CFC" != "no"; then
  dnl Write more examples of tests here...

  dnl # --with-cfc -> check with-path
  dnl SEARCH_PATH="/usr/local /usr"     # you might want to change this
  dnl SEARCH_FOR="/include/cfc.h"  # you most likely want to change this
  dnl if test -r $PHP_CFC/$SEARCH_FOR; then # path given as parameter
  dnl   CFC_DIR=$PHP_CFC
  dnl else # search default path list
  dnl   AC_MSG_CHECKING([for cfc files in default path])
  dnl   for i in $SEARCH_PATH ; do
  dnl     if test -r $i/$SEARCH_FOR; then
  dnl       CFC_DIR=$i
  dnl       AC_MSG_RESULT(found in $i)
  dnl     fi
  dnl   done
  dnl fi
  dnl
  dnl if test -z "$CFC_DIR"; then
  dnl   AC_MSG_RESULT([not found])
  dnl   AC_MSG_ERROR([Please reinstall the cfc distribution])
  dnl fi

  dnl # --with-cfc -> add include path
  dnl PHP_ADD_INCLUDE($CFC_DIR/include)

  dnl # --with-cfc -> check for lib and symbol presence
  dnl LIBNAME=cfc # you may want to change this
  dnl LIBSYMBOL=cfc # you most likely want to change this

  dnl PHP_CHECK_LIBRARY($LIBNAME,$LIBSYMBOL,
  dnl [
  PHP_ADD_LIBRARY_WITH_PATH(hiredis, $CFC_DIR/$PHP_LIB, CFC_SHARED_LIBADD)
  dnl   AC_DEFINE(HAVE_CFCLIB,1,[ ])
  dnl ],[
  dnl   AC_MSG_ERROR([wrong cfc lib version or lib not found])
  dnl ],[
  dnl   -L$CFC_DIR/$PHP_LIBDIR -lm
  dnl ])
  dnl
  PHP_SUBST(CFC_SHARED_LIBADD)

  PHP_NEW_EXTENSION(cfc, cfc.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
