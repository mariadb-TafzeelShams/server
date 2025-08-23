/*
   Copyright (c) 2025, MariaDB
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#ifndef SQL_JSON_LIB
#define SQL_JSON_LIB

/*
  A syntax sugar interface to json_string_t
*/
class Json_string
{
  json_string_t str;

public:
  explicit Json_string(const char *name)
  {
    json_string_set_str(&str, (const uchar *) name,
                        (const uchar *) name + strlen(name));
    json_string_set_cs(&str, system_charset_info);
  }
  json_string_t *get() { return &str; }
};

/*
  This [partially] saves the JSON parser state and then can rollback the parser
  to it.
  The goal of this is to be able to make multiple json_key_matches() calls:
    Json_saved_parser_state save(je);
    if (json_key_matches(je, KEY_NAME_1)) {
      ...
      return;
    }
    save.restore_to(je);
    if (json_key_matches(je, KEY_NAME_2)) {
      ...
    }
  This allows one to parse JSON objects where [optional] members come in any
  order.
*/
class Json_saved_parser_state
{
  const uchar *c_str;
  my_wc_t c_next;
  int state;

public:
  explicit Json_saved_parser_state(const json_engine_t *je)
      : c_str(je->s.c_str), c_next(je->s.c_next), state(je->state)
  {
  }
  void restore_to(json_engine_t *je)
  {
    je->s.c_str= c_str;
    je->s.c_next= c_next;
    je->state= state;
  }
};

/*
  @brief
    Un-escape a JSON string and save it into *out.
*/
bool json_unescape_to_string(const char *val, int val_len, String *out);

/*
  parse the json to read and put a string into the argument value
  fill in the err_buf if any error has occurred during parsing
  @return
    FALSE  OK
    TRUE  Parse Error
*/
static bool read_string(THD *thd, json_engine_t *je, const char *read_elem_key,
                        String *err_buf, char *&value)
{
  if (json_read_value(je))
  {
    err_buf->append(STRING_WITH_LEN("error reading "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" value"));
    return 1;
  }

  StringBuffer<128> val_buf;
  if (json_unescape_to_string((const char *) je->value, je->value_len,
                              &val_buf))
  {
    err_buf->append(STRING_WITH_LEN("un-escaping error of "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" element"));
    return 1;
  }

  value= strdup_root(thd->mem_root, val_buf.c_ptr_safe());
  return 0;
}

/*
  parse the json to read and put a double into the argument value
  fill in the err_buf if any error has occurred during parsing
  @return
    FALSE  OK
    TRUE  Parse Error
*/
static bool read_double(json_engine_t *je, const char *read_elem_key,
                        String *err_buf, double &value)
{
  if (json_read_value(je))
  {
    err_buf->append(STRING_WITH_LEN("error reading "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" value"));
    return 1;
  }

  const char *size= (const char *) je->value_begin;
  char *size_end= (char *) je->value_end;
  int conv_err;
  value= my_strtod(size, &size_end, &conv_err);
  if (conv_err)
  {
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" member must be a floating point value"));
    return 1;
  }
  return 0;
}

/*
  parse the json to read and put a ha_rows into the argument value
  fill in the err_buf if any error has occurred during parsing
  @return
    FALSE  OK
    TRUE  Parse Error
*/
static bool read_ha_rows(json_engine_t *je, const char *read_elem_key,
                         String *err_buf, ha_rows &value)
{
  if (json_read_value(je))
  {
    err_buf->append(STRING_WITH_LEN("error reading "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" value"));
    return 1;
  }

  const char *size= (const char *) je->value_begin;
  char *size_end= (char *) je->value_end;
  int conv_err;
  value= my_strtoll10(size, &size_end, &conv_err);
  if (conv_err)
  {
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" member must be a numeric value"));
    return 1;
  }
  return 0;
}

/*
  Interface to read a value with value_name from Json,
  while in the process of parsing the Json document
*/
class Read_value
{
public:
  virtual int read_value(json_engine_t *je, const char *value_name,
                         String *err_buf)= 0;
  virtual ~Read_value() {};
};

class Read_string : public Read_value
{
  char **ptr;
  THD *thd;

public:
  Read_string(THD *thd_arg, char **ptr_arg) : ptr(ptr_arg), thd(thd_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return read_string(thd, je, value_name, err_buf, *ptr);
  }
};

class Read_double : public Read_value
{
  double *ptr;

public:
  Read_double(double *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return read_double(je, value_name, err_buf, *ptr);
  }
};

class Read_ha_rows : public Read_value
{
  ha_rows *ptr;

public:
  Read_ha_rows(ha_rows *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return read_ha_rows(je, value_name, err_buf, *ptr);
  }
};

class Read_longlong : public Read_value
{
  longlong *ptr;

public:
  Read_longlong(longlong *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    ha_rows temp_val;
    int rc= read_ha_rows(je, value_name, err_buf, temp_val);
    if (rc)
    {
      err_buf->append(STRING_WITH_LEN("got a parse error while parsing "));
      err_buf->append(value_name, strlen(value_name));
      return 1;
    }
    else if (temp_val > LONGLONG_MAX)
    {
      err_buf->append(value_name, strlen(value_name));
      err_buf->append(STRING_WITH_LEN(" is out of range of longlong"));
      return 1;
    }
    *ptr= (longlong) temp_val;
    return rc;
  }
};

class Read_uint : public Read_value
{
  uint *ptr;

public:
  Read_uint(uint *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    ha_rows temp_val;
    int rc= read_ha_rows(je, value_name, err_buf, temp_val);
    if (rc)
    {
      err_buf->append(STRING_WITH_LEN("got a parse error while parsing "));
      err_buf->append(value_name, strlen(value_name));
      return 1;
    }
    else if (temp_val > UINT_MAX)
    {
      err_buf->append(value_name, strlen(value_name));
      err_buf->append(STRING_WITH_LEN(" is out of range of unsigned int"));
      return 1;
    }
    *ptr= (uint) temp_val;
    return rc;
  }
};

class Read_bool : public Read_value
{
  bool *ptr;

public:
  Read_bool(bool *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    ha_rows temp_val;
    int rc= read_ha_rows(je, value_name, err_buf, temp_val);
    if (rc)
    {
      err_buf->append(STRING_WITH_LEN("got a parse error while parsing "));
      err_buf->append(value_name, strlen(value_name));
      return 1;
    }
    else if (temp_val > 1)
    {
      err_buf->append(value_name, strlen(value_name));
      err_buf->append(STRING_WITH_LEN(" is out of range of boolean value"));
      return 1;
    }
    *ptr= temp_val == 1;
    return rc;
  }
};

#endif
