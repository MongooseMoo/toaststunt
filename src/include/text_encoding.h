
#ifndef TEXT_ENCODING_H
#define TEXT_ENCODING_H 1

int text_encoding_is_valid(const char *encoding, const char *str);

int text_encoding_convert(const char *from_encoding, const char *to_encoding, const char *str, char **out);

void register_text_encoding();

#endif