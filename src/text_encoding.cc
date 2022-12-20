/* Text encoding and decoding so that the MOO can deal with non-ascii strings*/

#include "text_encoding.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "utils.h"

#include <iconv.h>
#include "text_encoding.h"

/*      Function: text_encoding_init
 * Description: Initialize the text encoding system
 */

void text_encoding_init(void)
{
    // initialize iconv
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    if (cd == (iconv_t)-1)
    {
        errlog("text_encoding_init: iconv_open failed");
    }
    else
    {
        iconv_close(cd);
    }
}

/*      Function: text_encoding_is_valid
 * Description: Check if a string is valid in a given encoding
 * arguments:   encoding - the encoding to check
 *             str - the string to check
 */

int text_encoding_is_valid(const char *encoding, const char *str)
{

    iconv_t cd = iconv_open(encoding, encoding);
    if (cd == (iconv_t)-1)
    {
        return 0;
    }
    else
    {
        size_t inbytesleft = strlen(str);
        size_t outbytesleft = inbytesleft * 4;
        char *inbuf = (char *)str;
        char *outbuf = (char *)malloc(outbytesleft);
        size_t ret = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
        iconv_close(cd);
        free(outbuf);
        if (ret == (size_t)-1)
        {
            return 0;
        }
        else
        {
            return 1;
        }
    }
}

/* function: text_encoding_convert
 * description: Convert a string from one encoding to another
 * arguments:   from_encoding - the encoding of the input string
 *            to_encoding - the encoding to convert to
 *           str - the string to convert
 * out: the converted string
 *   returns: 1 on success, 0 on failure
 */

int text_encoding_convert(const char *from_encoding, const char *to_encoding, const char *str, char **out)
{
    iconv_t cd = iconv_open(to_encoding, from_encoding);
    if (cd == (iconv_t)-1)
    {
        return 0;
    }
    else
    {
        size_t inbytesleft = strlen(str);
        size_t outbytesleft = inbytesleft * 4;
        char *inbuf = (char *)str;
        char *outbuf = (char *)mymalloc(outbytesleft, M_STRING);
        char *outbuf_start = outbuf;
        size_t ret = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
        iconv_close(cd);
        if (ret == (size_t)-1)
        {
            myfree(outbuf_start, M_STRING);
            return 0;
        }
        else
        {
            *out = outbuf_start;
            *outbuf = '\0';
            return 1;
        }
    }
}

static package bf_text_encoding_is_valid(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = TYPE_INT;
    r.v.num = text_encoding_is_valid(arglist.v.list[1].v.str, arglist.v.list[2].v.str);
    free_var(arglist);
    return make_var_pack(r);
}

static package bf_encode_text(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    char *out;
    if (text_encoding_convert(arglist.v.list[2].v.str, arglist.v.list[3].v.str, arglist.v.list[1].v.str, &out))
    {
        r.type = TYPE_STR;
        r.v.str = out;
    }
    else
    {
        r.type = TYPE_ERR;
        r.v.err = E_INVARG;
    }
    free_var(arglist);
    return make_var_pack(r);
}

/* Function: bf_text_encodings
 * Description: Return a list of all supported text encodings from Iconv
 */

static package bf_text_encodings(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = TYPE_LIST;
    r.v.list = new_list(0);
    for (int i = 0; text_encodings[i]; i++)
    {
        r.v.list = listappend(r.v.list, str_dup_to_var(text_encodings[i]));
    }
    free_var(arglist);
    return make_var_pack(r);
}

void register_text_encoding()
{

    register_function("text_encoding_is_valid", 2, 2, bf_text_encoding_is_valid, TYPE_STR, TYPE_STR);
    register_function("encode_text", 3, 3, bf_encode_text, TYPE_STR, TYPE_STR, TYPE_STR);
}
