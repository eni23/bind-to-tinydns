/* bind-to-tinydns.c, version 0.4, 20030601
 * written by Daniel Erat <dan@erat.org> -- http://erat.org/ */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINE_LEN 8192
#define DOMAIN_LEN 255
#define MAX_TOKENS 32
#define MAX_PAREN 3
#define MAX_GEN_PARTS 10
#define DEFAULT_TTL 86400

#define DOMAIN_STR_LEN (DOMAIN_LEN * 4 + 1)

typedef struct string {
	char text[DOMAIN_STR_LEN];
	int len, real_len;
} string;

FILE *file = NULL;	/* file pointer for temp file */
char *filename = NULL;	/* filename of temp file */
int line_num = 1;	/* actual line num */
int start_line_num = 1;	/* line on which current entry started */

/* warning: prints a warning message to stderr */
void warning (const char *message, int line_number)
{
	if (line_number > 0)
		fprintf (stderr, "warning: line %d: %s\n",
			 line_number, message);
	else fprintf (stderr, "warning: %s\n", message);
}

/* fatal: prints an error message with line number, closes and unlinks temp
 * file if necessary, and exits */
void fatal (const char *message, int line_number)
{
	if (line_number > 0)
		fprintf (stderr, "fatal: line %d: %s\n",
			 line_number, message);
	else fprintf (stderr, "fatal: %s\n", message);

	if (file) {
		if (fclose (file)) {
			fprintf (stderr, "unable to close temp file: %s\n",
				 strerror (errno));
		} else if (unlink (filename)) {
			fprintf (stderr, "unable to unlink temp file: %s\n",
				 strerror (errno));
		}
	}
	exit (1);
}

/* sanitize_string: sanitizes the BIND-escaped string src and copies it to
 * the memory pointed to by dest.  a temporary string is used, so dest and
 * src can point to the same memory.  returns 0 on success and 1 otherwise.
 * */
int sanitize_string (string *dest, const char *src)
{
	string temp;

	if (!dest || !src) {
		warning ("sanitize_string: missing src or dest", -1);
		return 1;
	}

	for (temp.text[0] = '\0', temp.len = 0, temp.real_len = 0;
	     *src != '\0'; src++) {

		/* make sure temp isn't full */
		if (temp.len == DOMAIN_LEN) {
			warning ("sanitize_string: src string too long", -1);
			return 1;
		}

		/* if we have a normal printable character */
		if (isprint (*src) && *src != '\\' && *src != ':') {
			temp.text[temp.real_len] = *src;
			temp.len++;
			temp.real_len++;
		/* if we have a backslash escape */
		} else if (*src == '\\') {
			/* make sure that there's a following char */
			if (*(src+1) == '\0') {
				warning ("sanitize_string: backslash escape "
					 "in src with no escaped char", -1);
				return 1;
			}
			/* if the following char isn't a digit */
			if (!isdigit (*(src+1))) {
				/* if the following char needs to be
				 * handled specially */
				if (*(src+1) == ':' || *(src+1) == '\\' ||
				    *(src+1) == '.' || !isprint (*(src+1))) {
					sprintf (temp.text + temp.real_len,
						 "\\%03o", *(src+1));
					temp.len++;
					temp.real_len += 4;
				/* otherwise, just print it */
				} else {
					temp.text[temp.real_len] = *(src+1);
					temp.len++;
					temp.real_len++;
				}
				src++;
			} else {
				/* if we have an escaped decimal sequence */
				if (*(src+2) != '\0' && *(src+3) != '\0' &&
				    isdigit (*(src+2)) && isdigit (*(src+3))) {
					int num;
					num = (*(src+1) - '0') * 100 +
					      (*(src+2) - '0') * 10 +
					      (*(src+3) - '0');
					/* make sure it's not too large */
					if (num > 255) {
						warning ("sanitize_string: "
							 "escaped decimal "
							 "number too large",
							 -1);
						return 1;
					}
					/* if we can print it normally */
					if (isprint (num) && num != ':' &&
					    num != '.' && num != '\\') {
						temp.text[temp.real_len] = num;
						temp.real_len++;
						temp.len++;
					/* otherwise, print it as an
					 * escaped octal sequence */
					} else {
						sprintf (temp.text +
							 temp.real_len,
							 "\\%03o", num);
						temp.real_len += 4;
						temp.len++;
					}
					src += 3;
				} else {
					warning ("sanitize_string: malformed "
						 "escaped decimal sequence",
						 -1);
					return 1;
				}
			}
		/* non-escaped sequence, but we need to escape it */
		} else {
			sprintf (temp.text + temp.real_len, "\\%03o", *src);
			temp.real_len += 4;
			temp.len++;
		}
		temp.text[temp.real_len] = '\0';
	}

	memcpy (dest, &temp, sizeof (string));
	return 0;
}

/* qualify_domain: given char* name (in BIND format) and string origin
 * (which has already been passed through sanitize_string), constructs a
 * fully-qualified domain name and copies it to dest.  name and origin can
 * not both be empty.  trailing periods are removed.  a temporary string is
 * used, so dest can point to memory contained in either name or origin.  0
 * is returned on success, and 1 otherwise. */
int qualify_domain (string *dest, const char *name, const string *origin)
{
	string temp, sname;

	if (!dest || !name) {
		warning ("qualify_domain: missing dest or name", -1);
		return 1;
	}

	if (sanitize_string (&sname, name)) {
		warning ("qualify_domain: unable to sanitize name", -1);
		return 1;
	}

	/* if sname isn't empty */
	if (sname.len) {
		if (sname.text[0] == '.' && sname.text[1] != '\0') {
			warning ("qualify_domain: empty label", -1);
			return 1;
		}
		/* make sure sname doesn't have two dots in a row */
		if (strstr (sname.text, "..")) {
			warning ("qualify_domain: empty label", -1);
			return 1;
		}
		/* if sname is '@' */
		if (sname.text[0] == '@' && sname.text[1] == '\0') {
			if (!origin || !origin->len) {
				warning ("qualify_domain: name is '@', "
					 "origin is missing", -1);
				return 1;
			}
			memcpy (&temp, origin, sizeof (string));
		/* name is not '@' */
		} else {
			/* if sname is fully-qualified */
			if (sname.text[sname.real_len-1] == '.') {
				memcpy (&temp, &sname, sizeof (string));
			/* sname is not fully-qualified */
			} else {
				/* origin does not exist */
				if (!origin || !origin->len) {
					warning ("qualify_domain: name is not "
						 "fully qualified and origin "
						 "is missing", -1);
					return 1;
				}
				/* if the origin is just the root */
				if (origin->text[0] == '.' &&
				    origin->text[1] == '\0') {
					/* make sure it fits */
					if (sname.len + 1 > DOMAIN_LEN) {
						warning ("qualify_domain: "
							 "name is too "
							 "long", -1);
						return 1;
					}
					memcpy (&temp, &sname, sizeof (string));
					temp.text[sname.real_len] = '.';
					temp.text[sname.real_len+1] = '\0';
					temp.len++;
					temp.real_len++;
				/* origin is not just root */
				} else {
					if (sname.len + 1 +
					    origin->len > DOMAIN_LEN) {
						warning ("qualify_domain: "
							 "name plus origin "
							 "is too long", -1);
						return 1;
					}
					memcpy (&temp, &sname, sizeof (string));
					temp.text[sname.real_len] = '.';
					strcpy (temp.text + sname.real_len + 1,
						origin->text);
					temp.len += 1 + origin->len;
					temp.real_len += 1 + origin->real_len;
				}
			}
		}
	/* sname is empty */
	} else {
		/* if origin does not exist */
		if (!origin || !origin->len) {
			warning ("qualify_domain: name and origin are "
				 "both empty or missing", -1);
			return 1;
		}
		memcpy (&temp, origin, sizeof (string));
	}

	memcpy (dest, &temp, sizeof (string));
	return 0;
}

/* str_to_uint: converts the given string into an unsigned integer.  if
 * allow_time_fmt is set, allows BIND time-format strings such as
 * "2w1d2h5m6s".  does not check for overflow.  returns the converted
 * number, and puts 0 into the integer pointed at by ret on success and 1
 * otherwise. */
unsigned int str_to_uint (const char *string, int allow_time_fmt, int *ret)
{
	int in_time_fmt = 0, in_part = 0;
	unsigned int total = 0, part = 0;

	if (!string || *string == '\0') {
		warning ("str_to_uint: NULL or empty string", -1);
		*ret = 1;
		return 0;
	}

	for (; *string != '\0'; string++) {
		if (*string >= '0' && *string <= '9') {
			if (!in_part) in_part = 1;
			part *= 10;
			part += *string - '0';
		} else {
			if (!allow_time_fmt || !in_part) {
				/* don't print a warning here, since this
				 * function is also used to test if strings
				 * are TTLs */
				*ret = 1;
				return 0;
			}
			if (!in_time_fmt) in_time_fmt = 1;
			/* we actually want to overflow here if necessary,
			 * because BIND does too... */
			if (*string == 'w' || *string == 'W') {
				total += part * 86400 * 7;
			} else if (*string == 'd' || *string == 'D') {
				total += part * 86400;
			} else if (*string == 'h' || *string == 'H') {
				total += part * 60 * 60;
			} else if (*string == 'm' || *string == 'M') {
				total += part * 60;
			} else if (*string == 's' || *string == 'S') {
				total += part;
			} else {
				*ret = 1;
				return 0;
			}
			part = 0;
			in_part = 0;
		}
	}
	if (in_time_fmt && in_part) {
		warning ("str_to_uint: unfinished time string", -1);
		*ret = 1;
		return 0;
	}
	if (!in_time_fmt) total = part;

	*ret = 0;
	return total;
}

/* sanitize_ip: takes the dotted-decimal ip address in src and turns it
 * into a nicely-formatted ip address if possible.  dest must be 16
 * characters (or more).  things like 127.00000.0.1 are okay, but strings
 * with out-of-bounds octets are rejected.  returns 0 on success and 1
 * otherwise. */
int sanitize_ip (char *dest, const char *src) {

	char *dot;
	int i, total;

	if (!src) return 1;
	for (i = 0; i < 4; i++) {
		if (i < 3) dot = strchr (src, '.');
		else for (; *dot != '\0'; dot++);
		if (!dot || dot == src) return 1;
		for (total = 0; src < dot; src++) {
			if (*src < '0' || *src > '9') return 1;
			total *= 10;
			total += *src - '0';
		}
		if (total > 255) return 1;
		sprintf (dest, "%d", total);
		for (; *dest != '\0'; dest++);
		if (i < 3) *dest++ = '.';
		src = dot + 1;
	}
	return 0;
}

/* tokenize: tokenizes a line from stdin.  puts the tokens into the token
 * array and returns the number of tokens found, or -1 if the end of the
 * file was reached. */
int tokenize (char **token)
{
	static char line[LINE_LEN+1], *blank_token = " ";
	int in_doublequote = 0, paren_level = 0;
	int in_quote = 0, set_in_quote = 0, in_txt = 0, i = 0;
	int num_tokens = 0, in_token = 0, found_nonblank_token = 0;

	start_line_num = line_num;

	do {
		if (!fgets (line + i, LINE_LEN + 1 - i, stdin))
			return -1;

		/* tokenize the input and look for obvious syntax
		 * errors */
		for (; line[i] != '\0' ; i++) {

			if (i == LINE_LEN)
				fatal ("entry too long", start_line_num);

			if (set_in_quote) {
				in_quote = 1;
				set_in_quote = 0;
			}

			if (line[i] == '\\' && !in_quote) {
				set_in_quote = 1;
			} else if (in_quote && line[i] != '\n') {
				in_quote = 0;
				continue;
			}

			if (in_doublequote && line[i] != '"') {
				continue;
			}

			if (line[i] == ';') {
				line[i] = '\0';
				in_token = 0;
				break;
			} else if (line[i] == '(') {
				line[i] = '\0';
				in_token = 0;
				if (++paren_level > MAX_PAREN)
					fatal ("too many nested parentheses",
					       start_line_num);
			} else if (line[i] == ')') {
				line[i] = '\0';
				in_token = 0;
				paren_level--;
				if (paren_level < 0)
					fatal ("missing opening parenthesis",
						start_line_num);
			} else if (line[i] == '"') {
				line[i] = '\0';
				if (in_doublequote) {
					in_token = 0;
				} else {
					if (!in_txt) {
					if (num_tokens == 0 ||
					    strcasecmp (token[num_tokens-1],
					      		"txt"))
						fatal ("improper use of "
						"double-quotes (can only be "
						"used for TXT rdata)",
						start_line_num);
					in_txt = 1;
					}
					token[num_tokens] =
						line + i + 1;
					num_tokens++;
					in_token = 1;
				}
				in_doublequote = !in_doublequote;
			} else if ((line[i] == ' ' ||
				    line[i] == '\t')) {
				if (i == 0) {
					token[num_tokens] = blank_token;
					num_tokens++;
				} else {
					line[i] = '\0';
					in_token = 0;
				}
			} else if (line[i] == '\n' || line[i] == '\r') {
				line[i] = '\0';
				in_token = 0;
			} else if (!in_token) {
				if (num_tokens >= MAX_TOKENS)
					fatal ("too many tokens in RR",
					       start_line_num);
				token[num_tokens] = line + i;
				in_token = 1;
				num_tokens++;
				found_nonblank_token = 1;
			}
		}

		if (in_quote) warning ("hanging backslash at end of line; "
				       "pretending it was terminated",
				       start_line_num);
		if (in_doublequote) warning ("open doublequoted string at "
					     "end of line; pretending it "
					     "was closed", start_line_num);

		line_num++;

	} while (paren_level);

	if (paren_level) fatal ("open parentheses at end of file",
				start_line_num);

	return found_nonblank_token ? num_tokens : 0;
}

/* parse_gen_string: parses the LHS or RHS of a $GENERATE directive into
 * tokens. */
void parse_gen_string (char *line, char **parts, int *offsets,
		       int *widths, char *bases, int *num_parts)
{
	char *ptr;
	int in_quote;

	if (!line || !parts || !offsets || !widths || !bases || !num_parts)
		fatal ("parse_gen_string: NULL parameter", -1);

	for (ptr = line, parts[0] = line, *num_parts = 1, in_quote = 0;
	     *ptr != '\0'; ptr++) {
		if (in_quote) {
			in_quote = 0;
			continue;
		}
		else if (*ptr == '\\') {
			in_quote = 1;
			continue;
		}
		if (*ptr == '$') {
			if (*(ptr+1) == '$') {
				ptr++;
				continue;
			}
			if (*num_parts >= MAX_GEN_PARTS)
				fatal ("$GENERATE directive has too many "
				       "parts", start_line_num);
			*ptr = '\0';

			if (parts[*num_parts-1] == ptr)
				parts[*num_parts-1] = NULL;
			else parts[(*num_parts)++] = NULL;

			offsets[*num_parts-1] = 0;
			widths[*num_parts-1] = 0;
			bases[*num_parts-1] = 'd';

			if (*(ptr+1) == '{') {
				int found, neg = 1;
				ptr += 2;
				if (*ptr == '-') {
					neg = -1;
					ptr++;
				}
				for (found = 0; *ptr >= '0' && *ptr <= '9';
				     ptr++) {
					offsets[*num_parts-1] *= 10;
					offsets[*num_parts-1] += *ptr - '0';
					found = 1;
				}
				offsets[*num_parts-1] *= neg;
				if (!found || (*ptr != ',' && *ptr != '}'))
					fatal ("parse error in $GENERATE "
					       "curly braces (at offset)",
					       start_line_num);
				/* hopefully i won't goto hell for this */
				if (*ptr == '}') goto PARSE_GEN_STRING_DONE;
				for (ptr++, found = 0;
				     *ptr >= '0' && *ptr <= '9'; ptr++) {
					widths[*num_parts-1] *= 10;
					widths[*num_parts-1] += *ptr - '0';
					found = 1;
				}
				if (!found || (*ptr != ',' && *ptr != '}'))
					fatal ("parse error in $GENERATE "
					       "curly braces (at width)",
					       start_line_num);
				if (*ptr == '}') goto PARSE_GEN_STRING_DONE;
				ptr++;
				if (*ptr != 'd' && *ptr != 'o' &&
				    *ptr != 'x' && *ptr != 'X')
					fatal ("$GENERATE has invalid base",
					       start_line_num);
				bases[*num_parts-1] = *ptr;
				ptr++;
				if (*ptr != '}')
					fatal ("parse error in $GENERATE "
					       "(curly braces not closed "
					       "after base)", start_line_num);
PARSE_GEN_STRING_DONE:
			}
			if (*(ptr+1) != '\0') {
				if (*num_parts >= MAX_GEN_PARTS)
					fatal ("$GENERATE directive has too "
					"many parts", start_line_num);
				parts[(*num_parts)++] = ptr + 1;
			}
		}
	}
}

/* construct_gen_output: constructs a string for a $generate directive.
 * dest is the destination string of length DOMAIN_STR_LEN, parts is the
 * array of parts, num is the dimension of the array, and iter is the
 * current value of the directive's iterator. */
void construct_gen_output (char *dest, char **parts, int *offsets,
			   int *widths, char *bases, int num_parts, int iter)
{
	int i, ret;
	char *ptr, format[16];

	for (i = 0, ptr = dest; i < num_parts; i++) {
		if (parts[i]) {
			ret = snprintf (ptr, DOMAIN_STR_LEN - (ptr - dest),
					"%s", parts[i]);
		} else {
			snprintf (format, 16, "%%0%d%c", widths[i], bases[i]); 
			ret = snprintf (ptr, DOMAIN_STR_LEN - (ptr - dest),
					format, iter + offsets[i]);
		}
		if (ret < 0 || ptr + ret > dest + DOMAIN_STR_LEN)
			fatal ("$GENERATE directive constructed a token "
			       "that was too long", start_line_num);
		ptr += ret;
	}
}

/* handle_entry: parses and handles the given entry. */
int handle_entry (int num_tokens, const char **token, string *cur_origin,
		  const string *top_origin, int *ttl)
{
	int i, ret;

	if (!num_tokens) return 0;

	/* $ORIGIN */
	if (!strcasecmp (token[0], "$ORIGIN")) {
		if (num_tokens != 2)
			fatal ("$ORIGIN directive has wrong number "
			       "of arguments", start_line_num);
		if (qualify_domain (cur_origin, token[1], cur_origin))
			fatal ("choked on domain name in $ORIGIN statement",
			       start_line_num);
	/* $TTL */
	} else if (!strcasecmp (token[0], "$TTL")) {
		if (num_tokens != 2)
			fatal ("$TTL directive has wrong number of arguments",
			       start_line_num);
		*ttl = str_to_uint (token[1], 1, &ret);
		if (ret || *ttl > 2147483646)
			fatal ("invalid $TTL value", start_line_num);
	/* $GENERATE */
	} else if (!strcasecmp (token[0], "$GENERATE")) {
		int start, stop, step, found, num_lhs_parts, num_rhs_parts;
		char *lhs_parts[MAX_GEN_PARTS], *rhs_parts[MAX_GEN_PARTS];
		int lhs_offsets[MAX_GEN_PARTS], rhs_offsets[MAX_GEN_PARTS];
		int lhs_widths[MAX_GEN_PARTS], rhs_widths[MAX_GEN_PARTS];
		char lhs_bases[MAX_GEN_PARTS], rhs_bases[MAX_GEN_PARTS];
		char lhs_line[LINE_LEN+1], rhs_line[LINE_LEN+1];
		char lhs_str[DOMAIN_STR_LEN], rhs_str[DOMAIN_STR_LEN];
		char *gen_token[3];

		if (num_tokens != 5)
			fatal ("$GENERATE directive has wrong number "
			       "of arguments", start_line_num);
		if (strcasecmp (token[3], "PTR") &&
		    strcasecmp (token[3], "CNAME") &&
		    strcasecmp (token[3], "A") &&
		    strcasecmp (token[3], "NS"))
			fatal ("$GENERATE directive has unknown RR type",
			       start_line_num);

		gen_token[1] = (char *) token[3];

		/* read range */
		for (found = 0, start = 0, i = 0;
		     token[1][i] >= '0' && token[1][i] <= '9'; i++) {
			start *= 10;
			start += token[1][i] - '0';
			found = 1;
		}
		if (!found || token[1][i] != '-')
			fatal ("$GENERATE directive has invalid range "
			       "(unable to parse start)", start_line_num);
		for (found = 0, stop = 0, i++;
		     token[1][i] >= '0' && token[1][i] <= '9'; i++) {
			stop *= 10;
			stop += token[1][i] - '0';
			found = 1;
		}
		if (!found || (token[1][i] != '\0' && token[1][i] != '/'))
			fatal ("$GENERATE directive has invalid range "
			       "(unable to parse stop)", start_line_num);
		if (token[1][i] == '/') {
			for (found = 0, step = 0, i++;
			     token[1][i] >= '0' && token[1][i] <= '9'; i++) {
				step *= 10;
				step += token[1][i] - '0';
				found = 1;
			}
			if (!found || token[1][i] != '\0' || !step)
				fatal ("$GENERATE directive has invalid range "
				       "(unable to parse step)",
				       start_line_num);
		} else {
			step = 1;
		}

		/* parse lhs and rhs */
		strcpy (lhs_line, token[2]);
		strcpy (rhs_line, token[4]);
		parse_gen_string (lhs_line, lhs_parts, lhs_offsets,
				  lhs_widths, lhs_bases, &num_lhs_parts);
		parse_gen_string (rhs_line, rhs_parts, rhs_offsets,
				  rhs_widths, rhs_bases, &num_rhs_parts);

		/* pass generated lines back into this function */
		for (i = start; i <= stop; i += step) {
			construct_gen_output (lhs_str, lhs_parts, lhs_offsets,
					      lhs_widths, lhs_bases,
					      num_lhs_parts, i);
			construct_gen_output (rhs_str, rhs_parts, rhs_offsets,
					      rhs_widths, rhs_bases,
					      num_rhs_parts, i);
			gen_token[0] = lhs_str;
			gen_token[2] = rhs_str;

			handle_entry (3, (const char **) gen_token,
				      cur_origin, top_origin, ttl);
		}
	/* $INCLUDE */
	} else if (!strcasecmp (token[0], "$INCLUDE")) {
		fatal ("sorry, $INCLUDE directive is not implemented",
		       start_line_num);
	} else if (token[0][0] == '$') {
		fatal ("unknown $ directive", start_line_num);
	/* handle records */
	} else {
		int next;
		unsigned int local_ttl, temp_ttl;
		static string owner;
		static int prev_owner = 0;
		string rdomain;

		if (num_tokens < 3)
			fatal ("RR does not have enough tokens",
			       start_line_num);

		if (strcmp (token[0], " ")) {
			if (qualify_domain (&owner, token[0], cur_origin))
				fatal ("choked on owner name in RR",
				       start_line_num);
			if (top_origin->real_len > owner.real_len ||
			    strcasecmp (top_origin->text,
					owner.text + owner.real_len -
					top_origin->real_len) ||
			    (owner.real_len > top_origin->real_len &&
			     *(owner.text + owner.real_len -
			       top_origin->real_len - 1) != '.')) {
				warning ("ignoring out-of-zone data",
					 start_line_num);
				return 1;
			}
			prev_owner = 1;
		} else {
			if (!prev_owner) {
				fatal ("RR tried to inherit owner from "
				       "previous record, but there was no "
				       "previous RR", start_line_num);
			}
		}

		local_ttl = *ttl;

		/* process ttl and/or class, and find where type
		 * token is.  whose brilliant idea was it to let
		 * these two come in either order? */
		next = 1;
		temp_ttl = str_to_uint (token[1], 1, &ret);
		if (!ret) {
			if (local_ttl > 2147483646)
				fatal ("invalid TTL in RR", start_line_num);
			local_ttl = temp_ttl;
			if (!strcasecmp (token[2], "IN")) {
				next = 3;
			} else {
				next = 2;
			}
		} else if (!strcasecmp (token[1], "IN")) {
			temp_ttl = str_to_uint (token[2], 1, &ret);
			if (!ret) {
				if (local_ttl > 2147483646)
					fatal ("invalid TTL in RR",
					       start_line_num);
				local_ttl = temp_ttl;
				next = 3;
			} else {
				next = 2;
			}
		}

		/* SOA */
		if (!strcasecmp (token[next], "SOA")) {
			string rname;
			unsigned int serial, refresh, retry;
			unsigned int expire, minimum;
			if (num_tokens - next - 1 == 2)
				fatal ("wrong number of tokens in SOA RDATA "
				       "(perhaps an opening parenthesis is on "
				       "the next line instead of this one?)",
				       start_line_num);
			if (num_tokens - next - 1 != 7)
				fatal ("wrong number of tokens in SOA RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+1],
					    cur_origin))
				fatal ("choked on MNAME in SOA RDATA",
				       start_line_num);
			if (qualify_domain (&rname, token[next+2], cur_origin))
				fatal ("choked on RNAME in SOA RDATA",
				       start_line_num);
			serial = str_to_uint (token[next+3], 0, &ret);
			if (ret) fatal ("invalid SERIAL in SOA RDATA",
					start_line_num);
			refresh = str_to_uint (token[next+4], 1, &ret);
			if (ret) fatal ("invalid REFRESH in SOA RDATA",
					start_line_num);
			retry = str_to_uint (token[next+5], 1, &ret);
			if (ret) fatal ("invalid RETRY in SOA RDATA",
					start_line_num);
			expire = str_to_uint (token[next+6], 1, &ret);
			if (ret) fatal ("invalid EXPIRE in SOA RDATA",
					start_line_num);
			minimum = str_to_uint (token[next+7], 1, &ret);
			if (ret) fatal ("invalid MINIMUM in SOA RDATA",
					start_line_num);
			fprintf (file, "Z%s:%s:%s:%u:%u:%u:%u:%u\n",
				 owner.text, rdomain.text, rname.text,
				 serial, refresh, retry, expire, minimum);
		/* NS */
		} else if (!strcasecmp (token[next], "NS")) {
			if (num_tokens - next - 1 != 1)
				fatal ("wrong number of tokens in NS RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+1],
					    cur_origin))
				fatal ("choked on domain name in NS RDATA",
				       start_line_num);
			fprintf (file, "&%s::%s:%d\n", owner.text,
				 rdomain.text, local_ttl);
		/* MX */
		} else if (!strcasecmp (token[next], "MX")) {
			unsigned int priority;
			if (num_tokens - next - 1 != 2)
				fatal ("wrong number of tokens in MX RDATA",
				       start_line_num);
			priority = str_to_uint (token[next+1], 0, &ret);
			if (ret || priority > 65535)
				fatal ("invalid priority in MX RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+2],
					    cur_origin))
				fatal ("choked on domain name in MX RDATA",
				       start_line_num);
			fprintf (file, "@%s::%s:%d:%d\n", owner.text,
				 rdomain.text, priority, local_ttl);
		/* A */
		} else if (!strcasecmp (token[next], "A")) {
			char ip[16];
			if (num_tokens - next - 1 != 1)
				fatal ("wrong number of tokens in A RDATA",
				       start_line_num);
			if (sanitize_ip (ip, token[next+1]))
				fatal ("invalid IP address in A RDATA",
				       start_line_num);
			fprintf (file, "+%s:%s:%d\n", owner.text,
				 ip, local_ttl);
		/* CNAME */
		} else if (!strcasecmp (token[next], "CNAME")) {
			if (num_tokens - next - 1 != 1)
				fatal ("wrong number of tokens in CNAME RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+1],
					    cur_origin))
				fatal ("choked on domain name in CNAME RDATA",
				       start_line_num);
			fprintf (file, "C%s:%s:%d\n", owner.text,
				 rdomain.text, local_ttl);
		/* PTR */
		} else if (!strcasecmp (token[next], "PTR")) {
			if (num_tokens - next - 1 != 1)
				fatal ("wrong number of tokens in PTR RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+1],
					    cur_origin))
				fatal ("choked on domain name in PTR RDATA",
				       start_line_num);
			fprintf (file, "^%s:%s:%d\n", owner.text,
				 rdomain.text, local_ttl);
		/* TXT */
		} else if (!strcasecmp (token[next], "TXT")) {
			string txt_rdata;
			if (num_tokens - next - 1 < 1)
				fatal ("too few tokens in TXT RDATA",
				       start_line_num);
			fprintf (file, ":%s:16:", owner.text);
			for (i = next + 1; i < num_tokens; i++) {
				if (sanitize_string (&txt_rdata, token[i]))
					fatal ("choked while sanitizing TXT "
					       "RDATA", start_line_num);
				fprintf (file, "\\%03o%s", txt_rdata.len,
					 txt_rdata.text);
			}
			fprintf (file, ":%d\n", local_ttl);
		/* SRV */
		} else if (!strcasecmp (token[next], "SRV")) {
			unsigned int priority, weight, port;
			if (num_tokens - next - 1 != 4)
				fatal ("wrong number of tokens "
				       "in SRV RDATA", start_line_num);
			priority = str_to_uint (token[next+1], 0, &ret);
			if (ret || priority > 65535)
				fatal ("invalid priority in SRV RDATA",
				       start_line_num);
			weight = str_to_uint (token[next+2], 0, &ret);
			if (ret || weight > 65535)
				fatal ("invalid weight in SRV RDATA",
				       start_line_num);
			port = str_to_uint (token[next+3], 0, &ret);
			if (ret || port > 65535)
				fatal ("invalid port in SRV RDATA",
				       start_line_num);
			if (qualify_domain (&rdomain, token[next+4],
					    cur_origin))
				fatal ("choked on domain name in SRV "
				       "RDATA", start_line_num);
			fprintf (file, ":%s:33:\\%03o\\%03o"
				 "\\%03o\\%03o\\%03o\\%03o\\%03o%s"
				 ":%d\n", owner.text, priority / 256,
				 priority % 256, weight / 256, weight % 256,
				 port / 256, port % 256, rdomain.len,
				 rdomain.text, local_ttl);
		/* other */
		} else {
			fatal ("unknown RR type", start_line_num);
		}
	}

	return 0;
}

/* main: */
int main (int argc, char *argv[])
{
	char *token[MAX_TOKENS];
	int fd, num_tokens;
	string origin, cur_origin;
	unsigned int ttl = DEFAULT_TTL;

	if (argc != 4) {
		fprintf (stderr, "bind-to-tinydns: usage: bind-to-tinydns "
			 "<origin> <output file> <temp file>\n");
		exit (1);
	}

	/* init origin */
	origin.text[0] = '.';
	origin.text[1] = '\0';
	origin.len = origin.real_len = 1;
	if (qualify_domain (&origin, argv[1], &origin))
		fatal ("unable to qualify initial origin", -1);
	memcpy (&cur_origin, &origin, sizeof (string));

	/* open temp file */
	filename = argv[3];
	if ((fd = open (filename, O_WRONLY | O_CREAT | O_EXCL, 0644)) == -1) {
		fprintf (stderr, "fatal: unable to create temp file: %s\n",
			 strerror (errno));
		exit (1);
	}
	if (!(file = fdopen (fd, "w"))) {
		fprintf (stderr, "fatal: unable to create file stream: %s\n",
			 strerror (errno));
		exit (1);
	}

	/* tokenize, parse, and emit each entry */
	while ((num_tokens = tokenize (token)) >= 0)
		handle_entry (num_tokens, (const char **) token,
			      &cur_origin, &origin, &ttl);

	/* close and rename temp file */
	if (fclose (file)) {
		fprintf (stderr, "fatal: unable to close temp file: %s\n",
			 strerror (errno));
		exit (1);
	}
	if (rename (filename, argv[2])) {
		fprintf (stderr, "fatal: unable to rename temp file: %s\n",
			 strerror (errno));
		if (unlink (filename)) {
			fprintf (stderr, "unable to unlink temp file: %s\n",
				 strerror (errno));
		}
		exit (1);
	}

	return 0;
}
