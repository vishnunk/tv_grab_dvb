#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "dvb_charset_tables.h"

static int convert_iso_8859[16] = {
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -1, 11, 12, 13
};
#define convert_utf8   14
#define convert_iso6937 15

#define PL_FIX_CHARS \
"\xA1\xA3\xA6\xAC\xAF\xB1\xB3\xB6\xBC\xBF"\
"\xC1\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xCA\xCB\xCD\xCE\xCF"\
"\xD1\xD3\xE6\xE8\xEA\xF1\xF3\xF8"

char *iso6937_encoding = "ISO6937";

static inline int encode_utf8(unsigned int c, char *outb, int outleft)
{
  if (c <= 0x7F && outleft >= 1) {
    *outb = c;
    return 1;
  } else if (c <= 0x7FF && outleft >=2) {
    *outb++ = ((c >>  6) & 0x1F) | 0xC0;
    *outb++ = ( c        & 0x3F) | 0x80;
    return 2;
  } else if (c <= 0xFFFF && outleft >= 3) {
    *outb++ = ((c >> 12) & 0x0F) | 0xE0;
    *outb++ = ((c >>  6) & 0x3F) | 0x80;
    *outb++ = ( c        & 0x3F) | 0x80;
    return 3;
  } else if (c <= 0x10FFFF && outleft >= 4) {
    *outb++ = ((c >> 18) & 0x07) | 0xF0;
    *outb++ = ((c >> 12) & 0x3F) | 0x80;
    *outb++ = ((c >>  6) & 0x3F) | 0x80;
    *outb++ = ( c        & 0x3F) | 0x80;
    return 4;
  } else {
    return -1;
  }
}

static inline size_t conv_utf8(const uint8_t *src, size_t srclen,
                              char *dst, size_t *dstlen)
{
  while (srclen>0 && (*dstlen)>0) {
    *dst = (char) *src;
    srclen--; (*dstlen)--;
    src++; dst++;
  }
  if (srclen>0) {
    errno = E2BIG;
    return -1;
  }
  return 0;
}

static inline size_t conv_8859(int conv,
                              const uint8_t *src, size_t srclen,
                              char *dst, size_t *dstlen)
{
  uint16_t *table = conv_8859_table[conv];

  while (srclen>0 && (*dstlen)>0) {
    uint8_t c = *src;
    if (c <= 0x7f) {
      // lower half of iso-8859-* is identical to utf-8
      *dst = (char) *src;
      (*dstlen)--;
      dst++;
    } else if (c <= 0x9f) {
      // codes 0x80 - 0x9f (control codes) are mapped to ' '
      *dst = ' ';
      (*dstlen)--;
      dst++;
    } else {
      // map according to character table, skipping
      // unmapped chars (value 0 in the table)
      uint16_t uc = table[c-0xa0];
      if (uc != 0) {
        int len = encode_utf8(uc, dst, *dstlen);
        if (len == -1) {
          errno = E2BIG;
          return -1;
        } else {
          (*dstlen) -= len;
          dst += len;
        }
      }
    }
    srclen--;
    src++;
  }
  if (srclen>0) {
    errno = E2BIG;
    return -1;
  }
  return 0;
}

static inline size_t conv_6937(const uint8_t *src, size_t srclen,
                              char *dst, size_t *dstlen)
{
  while (srclen>0 && (*dstlen)>0) {
    uint8_t c = *src;
    if (c <= 0x7f) {
      // lower half of iso6937 is identical to utf-8
      *dst = (char) *src;
      (*dstlen)--;
      dst++;
    } else if (c <= 0x9f) {
      // codes 0x80 - 0x9f (control codes) are mapped to ' '
      *dst = ' ';
      (*dstlen)--;
      dst++;
    } else {
      uint16_t uc;
      if (c >= 0xc0 && c <= 0xcf) {
        // map two-byte sequence, skipping illegal combinations.
        if (srclen<2) {
          errno = EINVAL;
          return -1;
        }
        srclen--;
        src++;
        uint8_t c2 = *src;
        if (c2 == 0x20) {
          uc = iso6937_lone_accents[c-0xc0];
        } else if (c2 >= 0x41 && c2 <= 0x5a) {
          uc = iso6937_multi_byte[c-0xc0][c2-0x41];
        } else if (c2 >= 0x61 && c2 <= 0x7a) {
          uc = iso6937_multi_byte[c-0xc0][c2-0x61+26];
        } else {
          uc = 0;
        }
      } else {
        // map according to single character table, skipping
        // unmapped chars (value 0 in the table)
        uc = iso6937_single_byte[c-0xa0];
      }
      if (uc != 0) {
        int len = encode_utf8(uc, dst, *dstlen);
        if (len == -1) {
          errno = E2BIG;
          return -1;
        } else {
          (*dstlen) -= len;
          dst += len;
        }
      }
    }
    srclen--;
    src++;
  }
  if (srclen>0) {
    errno = E2BIG;
    return -1;
  }
  return 0;
}

static inline size_t dvb_convert(int conv,
                          const uint8_t *src, size_t srclen,
                          char *dst, size_t *dstlen)
{
  switch (conv) {
    case convert_utf8: return conv_utf8(src, srclen, dst, dstlen);
    case convert_iso6937: return conv_6937(src, srclen, dst, dstlen);
    default: return conv_8859(conv, src, srclen, dst, dstlen);
  }
}

/*
 * DVB String conversion according to EN 300 468, Annex A
 * Not all character sets are supported, but it should cover most of them
 */

int
dvb_get_string(char *dst, size_t dstlen, const uint8_t *src, size_t srclen, char *dvb_default_charset)
{
  int ic;
  size_t len, outlen;
  int i;

  if(srclen < 1) {
    *dst = 0;
    return 0;
  }

  switch(src[0]) {
  case 0:
    return -1;

  case 0x01 ... 0x0b:
    // NOTE: some Polish channels using ISO6937 while specifying ISO8859-4
    if ((src[0] + 4) == 5 && strpbrk((const char*)src, PL_FIX_CHARS))
      ic = convert_iso6937;
    else
      ic = convert_iso_8859[src[0] + 4];
    src++; srclen--;
    break;

  case 0x0c ... 0x0f:
    return -1;

  case 0x10: /* Table A.4 */
    if(srclen < 3 || src[1] != 0 || src[2] == 0 || src[2] > 0x0f)
      return -1;

    ic = convert_iso_8859[src[2]];
    src+=3; srclen-=3;
    break;
    
  case 0x11 ... 0x14:
    return -1;

  case 0x15:
    ic = convert_utf8;
    break;
  case 0x16 ... 0x1f:
    return -1;

  default:
    if (dvb_default_charset != NULL && sscanf(dvb_default_charset, "ISO8859-%d", &i) > 0) {
      if (i > 0 && i < 16) {
        ic = convert_iso_8859[i];
      } else {
        ic = convert_iso6937;
      }
    } else {
      // Note: some Polish channels using ISO8859-2 while specifying no encoding
      if (strpbrk((const char*)src, PL_FIX_CHARS))
        ic = convert_iso_8859[2];
      else
        ic = convert_iso6937;
    }
    break;
  }

  if(srclen < 1) {
    *dst = 0;
    return 0;
  }

  if(ic == -1)
    return -1;

  outlen = dstlen - 1;
  //ic = convert_iso_8859[2];
  if (dvb_convert(ic, src, srclen, dst, &outlen) == -1) {
    return -1;
  }

  len = dstlen - outlen - 1;
  dst[len] = 0;
  return 0;
}



/* Quote the xml entities in the string passed in.
 * NB this is returned as a pointer to a string on the heap which will
 * be re-used on the next call to xmlify()
 * Patched by Nick Craig-Wood - nick craig-wood com for more chars with
 * strcat func for tidyness
 */
char *xmlify(const char *s, int len) {
	static char *xml = NULL;
	static unsigned bufsz = 0;
	int max_len = len * 10 + 1; /* Max possible expansion of string n * &quot; + NULL */
		                         /* A little untidy but fast! */

	/*  Patch by Steve Davies <steve one47 co uk> for better memory management */
	if (bufsz < max_len || xml == NULL) {
		xml = realloc(xml, max_len);
		bufsz = max_len;
	}
	if (dvb_get_string(xml, bufsz, s, len+1, iso6937_encoding))
		return s;	
	else
		return xml;
}

#ifdef MAIN
int main(int argc, char **argv) {
	if (argc > 1)
		printf("%s\n%s\n", argv[1], xmlify(argv[1]));
	return 0;
}
#endif
// vim: foldmethod=marker
