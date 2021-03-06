/* Include the required headers from httpd */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0400
#include <Windows.h>
#include <winsock2.h>
#include <objbase.h>
#include <ws2tcpip.h>

#include <apr.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_md5.h>
#include <apr_sha1.h>
#include <apr_base64.h>
#include <apr_dbd.h>
#include <apr_file_info.h>
#include <apr_file_io.h>
#include <apr_tables.h>
#include <apr_fnmatch.h>

#include <httpd.h>
#include <http_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_main.h>
#include <http_protocol.h>
#include <http_request.h>
#include <util_script.h>

#include <string.h>

/* apreq2 */
#include "apreq2/apreq_module_apache2.h"

/* hoedown */
#include "hoedown/src/version.h"
#include "hoedown/src/document.h"
#include "hoedown/src/html.h"
#include "hoedown/src/buffer.h"

#ifdef HAVE_CONFIG_H
#  undef PACKAGE_NAME
#  undef PACKAGE_STRING
#  undef PACKAGE_TARNAME
#  undef PACKAGE_VERSION
#  include "config.h"
#endif

#ifdef HOEDOWN_URL_SUPPORT
/* libcurl */
#include "curl/curl.h"
#endif

#ifdef _MSC_VER 
//not #if defined(_WIN32) || defined(_WIN64) because we have strncasecmp in mingw
#define rawmemchr memchr
#define rawmemchr memchr
#endif

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif;

static char* MODULE_NAME = "mod_hoedown";
static char* MODULE_TITLE = "Hoedown Module";
static char* MODULE_HANDLER_TITLE = "Hoedown";

#define HOEDOWN_READ_UNIT       1024
#define HOEDOWN_OUTPUT_UNIT     64
#define HOEDOWN_CURL_TIMEOUT    30
#define HOEDOWN_TITLE_DEFAULT   "Markdown"
#define HOEDOWN_TITLE_MARKER    "$title"
#define HOEDOWN_CONTENT_TYPE    "text/html"
#define HOEDOWN_TAG             "<body*>"
#define HOEDOWN_STYLE_EXT       ".html"
#define HOEDOWN_DIRECTORY_INDEX "index.md"
#define HOEDOWN_TOC_BEGIN        2
#define HOEDOWN_TOC_END          6


/* Define prototypes of our functions in this module */
extern module AP_MODULE_DECLARE_DATA hoedown_module;
static void hoedown_register_hooks(apr_pool_t* pool);
static int hoedown_handler(request_rec* r);

/********************************************************
*	CONFIGURATION PROTOTYPE								*
********************************************************/
typedef struct {
	char* default_page;
	char* directory_index;
	struct {
		char* path;
		char* name;
		char* ext;
	} style;
	struct {
		char* ul;
		char* ol;
		char* task;
	} class;
	struct {
		int begin;
		int end;
		int unescape;
		char* header;
		char* footer;
	} toc;
	int raw;
	unsigned int extensions;
	unsigned int html;
} hoedown_config;

#define HOEDOWN_SET_EXTENSIONS(_name, _ext) \
static const char * \
hoedown_set_extensions_ ## _name( \
    cmd_parms * UNUSED(parms), void *mconfig, int bool) { \
    hoedown_config *cfg = (hoedown_config *)mconfig; \
    if (bool != 0) { \
        cfg->extensions |= _ext; \
    } else { \
        cfg->extensions ^= _ext; \
    } \
    return NULL; \
}

#define HOEDOWN_SET_RENDER(_name, _ext) \
static const char * \
hoedown_set_render_ ## _name( \
    cmd_parms * UNUSED(parms), void *mconfig, int bool) { \
    hoedown_config *cfg = (hoedown_config *)mconfig; \
    if (bool != 0) { \
        cfg->html |= _ext; \
    } else { \
        cfg->html ^= _ext; \
    } \
    return NULL; \
}

HOEDOWN_SET_EXTENSIONS(spaceheaders, HOEDOWN_EXT_SPACE_HEADERS);
HOEDOWN_SET_EXTENSIONS(tables, HOEDOWN_EXT_TABLES);
HOEDOWN_SET_EXTENSIONS(fencedcode, HOEDOWN_EXT_FENCED_CODE);
HOEDOWN_SET_EXTENSIONS(footnotes, HOEDOWN_EXT_FOOTNOTES);
HOEDOWN_SET_EXTENSIONS(autolink, HOEDOWN_EXT_AUTOLINK);
HOEDOWN_SET_EXTENSIONS(strikethrough, HOEDOWN_EXT_STRIKETHROUGH);
HOEDOWN_SET_EXTENSIONS(underline, HOEDOWN_EXT_UNDERLINE);
HOEDOWN_SET_EXTENSIONS(highlight, HOEDOWN_EXT_HIGHLIGHT);
HOEDOWN_SET_EXTENSIONS(quote, HOEDOWN_EXT_QUOTE);
HOEDOWN_SET_EXTENSIONS(superscript, HOEDOWN_EXT_SUPERSCRIPT);
HOEDOWN_SET_EXTENSIONS(laxspacing, HOEDOWN_EXT_LAX_SPACING);
HOEDOWN_SET_EXTENSIONS(nointraemphasis, HOEDOWN_EXT_NO_INTRA_EMPHASIS);
HOEDOWN_SET_EXTENSIONS(disableindentedcode, HOEDOWN_EXT_DISABLE_INDENTED_CODE);
#ifdef HOEDOWN_VERSION_EXTRAS
HOEDOWN_SET_EXTENSIONS(specialattribute, HOEDOWN_EXT_SPECIAL_ATTRIBUTE);
#endif

HOEDOWN_SET_RENDER(skiphtml, HOEDOWN_HTML_SKIP_HTML);
HOEDOWN_SET_RENDER(skipstyle, HOEDOWN_HTML_SKIP_STYLE);
HOEDOWN_SET_RENDER(skipimages, HOEDOWN_HTML_SKIP_IMAGES);
HOEDOWN_SET_RENDER(skiplinks, HOEDOWN_HTML_SKIP_LINKS);
HOEDOWN_SET_RENDER(expandtabs, HOEDOWN_HTML_EXPAND_TABS);
HOEDOWN_SET_RENDER(safelink, HOEDOWN_HTML_SAFELINK);
HOEDOWN_SET_RENDER(toc, HOEDOWN_HTML_TOC);
HOEDOWN_SET_RENDER(hardwrap, HOEDOWN_HTML_HARD_WRAP);
HOEDOWN_SET_RENDER(usexhtml, HOEDOWN_HTML_USE_XHTML);
HOEDOWN_SET_RENDER(escape, HOEDOWN_HTML_ESCAPE);
#ifdef HOEDOWN_VERSION_EXTRAS
HOEDOWN_SET_RENDER(usetasklist, HOEDOWN_HTML_USE_TASK_LIST);
HOEDOWN_SET_RENDER(linecontinue, HOEDOWN_HTML_LINE_CONTINUE);
#endif


#ifdef HOEDOWN_URL_SUPPORT
static size_t
append_url_data(void* buffer, size_t size, size_t nmemb, void* user) {
	size_t segsize = size * nmemb;
	append_data((hoedown_buffer*)user, buffer, segsize);
	return segsize;
}
#endif

static int output_style_header(request_rec* r, apr_file_t* fp, char const* markdown_title) {
	char buf[HUGE_STRING_LEN];
	char* lower = NULL;

	while(apr_file_gets(buf, HUGE_STRING_LEN, fp) == APR_SUCCESS) {
		char* p = strstr(buf, HOEDOWN_TITLE_MARKER);
		if(!p) {
			ap_rputs(buf, r);
		} else {
			ap_rwrite(buf, p - buf, r);
			ap_rputs(markdown_title, r);
			ap_rputs(p + strlen(HOEDOWN_TITLE_MARKER), r);
		}

		lower = apr_pstrdup(r->pool, buf);
		ap_str_tolower(lower);
		if(apr_fnmatch("*"HOEDOWN_TAG"*", lower, APR_FNM_CASE_BLIND) == 0) {
			return 1;
		}
	}

	return 0;
}

static apr_file_t* style_header(request_rec* r, hoedown_config* cfg, char const* style_filename, char const* markdown_filename) {
	apr_status_t rc = -1;
	apr_file_t* fp = NULL;
	char* style_filepath = NULL;
	char* markdown_title;

	if(markdown_filename) {
		char const* p, * pp, * ps;
		p = pp = ps = rawmemchr(markdown_filename, '\0', (size_t)-1);
		while(ps >= markdown_filename && *ps != '/') {
			ps--;
		}
		if(ps <= markdown_filename) {
			ps = markdown_filename;
		} else {
			ps++;
		}
		while(p >= ps && *p != '.') {
			p--;
		}
		if(p < ps) {
			p = pp;
		}
		markdown_title = apr_pstrndup(r->pool, ps, p - ps);
	} else {
		markdown_title = HOEDOWN_TITLE_DEFAULT;
	}

	if(style_filename == NULL && cfg->style.name != NULL) {
		style_filename = cfg->style.name;
	}

	if(style_filename != NULL) {
		if(cfg->style.path == NULL) {
			ap_add_common_vars(r);
			cfg->style.path = (char*)apr_table_get(r->subprocess_env,
												   "DOCUMENT_ROOT");
		}

		style_filepath = apr_psprintf(r->pool, "%s/%s%s",
									  cfg->style.path, style_filename,
									  cfg->style.ext);

		rc = apr_file_open(&fp, style_filepath,
						   APR_READ | APR_BINARY | APR_XTHREAD,
						   APR_OS_DEFAULT, r->pool);
		if(rc == APR_SUCCESS) {
			if(output_style_header(r, fp, markdown_title) != 1) {
				apr_file_close(fp);
				fp = NULL;
			}
		} else {
			style_filepath = apr_psprintf(r->pool, "%s/%s%s",
										  cfg->style.path,
										  cfg->style.name,
										  cfg->style.ext);

			rc = apr_file_open(&fp, style_filepath,
							   APR_READ | APR_BINARY | APR_XTHREAD,
							   APR_OS_DEFAULT, r->pool);
			if(rc == APR_SUCCESS) {
				if(output_style_header(r, fp, markdown_title) != 1) {
					apr_file_close(fp);
					fp = NULL;
				}
			}
		}
	}

	if(rc != APR_SUCCESS) {
		ap_rputs("<!DOCTYPE html>\n<html>\n", r);
		ap_rprintf(r, "<head><title>%s</title></head>\n", markdown_title);
		ap_rputs("<body>\n", r);
	}

	return fp;
}

static int style_footer(request_rec* r, apr_file_t* fp) {
	char buf[HUGE_STRING_LEN];

	if(fp != NULL) {
		while(apr_file_gets(buf, HUGE_STRING_LEN, fp) == APR_SUCCESS) {
			ap_rputs(buf, r);
		}
		apr_file_close(fp);
	} else {
		ap_rputs("</body>\n</html>\n", r);
	}

	return 0;
}

static void append_data(hoedown_buffer* ib, int* buffer, size_t size) {
	size_t offset = 0;

	if(!ib || !buffer || size == 0) {
		return;
	}

	while(offset < size) {
		size_t bufsize = ib->asize - ib->size;
		if(size >= (bufsize + offset)) {
			memcpy(ib->data + ib->size, buffer + offset, bufsize);
			ib->size += bufsize;
			hoedown_buffer_grow(ib, ib->size + HOEDOWN_READ_UNIT);
			offset += bufsize;
		} else {
			bufsize = size - offset;
			if(bufsize > 0) {
				memcpy(ib->data + ib->size, buffer + offset, bufsize);
				ib->size += bufsize;
			}
			break;
		}
	}
}

static int append_page_data(request_rec* r, hoedown_config* cfg, hoedown_buffer* ib, char* name, int directory) {
	apr_status_t rc = -1;
	apr_file_t* fp = NULL;
	apr_size_t read;
	char* filename = NULL;

	if(name == NULL) {
		if(!cfg->default_page) {
			return HTTP_NOT_FOUND;
		}
		filename = cfg->default_page;
	} else if(strlen(name) <= 0 ||
			  memcmp(name + strlen(name) - 1, "/", 1) == 0) {
		if(!cfg->directory_index || !directory) {
			return HTTP_FORBIDDEN;
		}
		filename = apr_psprintf(r->pool, "%s%s", name, cfg->directory_index);
	} else {
		filename = name;
	}

	rc = apr_file_open(&fp, filename,
					   APR_READ | APR_BINARY | APR_XTHREAD, APR_OS_DEFAULT,
					   r->pool);
	if(rc != APR_SUCCESS || !fp) {
		switch(errno) {
			case ENOENT:
				return HTTP_NOT_FOUND;
			case EACCES:
				return HTTP_FORBIDDEN;
			default:
				break;
		}
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	do {
		rc = apr_file_read_full(fp, ib->data + ib->size, ib->asize - ib->size,
								&read);
		if(read > 0) {
			ib->size += read;
			hoedown_buffer_grow(ib, ib->size + HOEDOWN_READ_UNIT);
		}
	} while(rc != APR_EOF);

	apr_file_close(fp);

	return APR_SUCCESS;
}


/********************************************************
*	DIRECTIVE STRUCTURE									*
********************************************************/
static const command_rec hoedown_directives[] = {
	AP_INIT_TAKE1("HoedownDefaultPage", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, default_page), OR_ALL, "hoedown default page file"),
	AP_INIT_TAKE1("HoedownDirectoryIndex", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, directory_index), OR_ALL, "hoedown directory index page"),
	
	/* Style file */
	AP_INIT_TAKE1("HoedownStylePath", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, style.path), OR_ALL, "hoedown style path"),
	AP_INIT_TAKE1("HoedownStyleDefault", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, style.name), OR_ALL, "hoedown default style file name"),
	AP_INIT_TAKE1("HoedownStyleExtension", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, style.ext), OR_ALL, "hoedown default style file extension"),
	
	#ifdef HOEDOWN_VERSION_EXTRAS
	/* Class name */
	AP_INIT_TAKE1("HoedownClassUl", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, class.ul), OR_ALL, "hoedown ul class attributes"),
	AP_INIT_TAKE1("HoedownClassOl", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, class.ol), OR_ALL, "hoedown ol class attributes"),
	AP_INIT_TAKE1("HoedownClassTask", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, class.task), OR_ALL, "hoedown task list class attributes"),
	#endif
	
	/* Toc options */
	AP_INIT_TAKE1("HoedownTocBegin", ap_set_int_slot, (void*)APR_OFFSETOF(hoedown_config, toc.begin), OR_ALL, "hoedown toc begin level"),
	AP_INIT_TAKE1("HoedownTocEnd", ap_set_int_slot, (void*)APR_OFFSETOF(hoedown_config, toc.end), OR_ALL, "hoedown toc end level"),
	#ifdef HOEDOWN_VERSION_EXTRAS
	AP_INIT_TAKE1("HoedownTocHeader", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, toc.header), OR_ALL, "hoedown toc header"),
	AP_INIT_TAKE1("HoedownTocFooter", ap_set_string_slot, (void*)APR_OFFSETOF(hoedown_config, toc.footer), OR_ALL, "hoedown toc footer"),
	AP_INIT_FLAG("HoedownTocUnescape", ap_set_flag_slot, (void*)APR_OFFSETOF(hoedown_config, toc.unescape), OR_ALL, "hoedown toc unescape"),
	#endif

	/* Raw options */
	AP_INIT_FLAG("HoedownRaw", ap_set_flag_slot,
	(void*)APR_OFFSETOF(hoedown_config, raw), OR_ALL, "Enable hoedown raw support"),

	/* Markdown extension options */
	AP_INIT_FLAG("HoedownExtSpaceHeaders", hoedown_set_extensions_spaceheaders, NULL, OR_ALL, "Enable hoedown extension Space Headers"),
	AP_INIT_FLAG("HoedownExtTables", hoedown_set_extensions_tables, NULL, OR_ALL, "Enable hoedown extension Tables"),
	AP_INIT_FLAG("HoedownExtFencedCode", hoedown_set_extensions_fencedcode, NULL, OR_ALL, "Enable hoedown extension Fenced Code"),
	AP_INIT_FLAG("HoedownExtFootnotes", hoedown_set_extensions_footnotes, NULL, OR_ALL, "Enable hoedown extension Footnotes"),
	AP_INIT_FLAG("HoedownExtAutolink", hoedown_set_extensions_autolink, NULL, OR_ALL, "Enable hoedown extension Autolink"),
	AP_INIT_FLAG("HoedownExtStrikethrough", hoedown_set_extensions_strikethrough, NULL, OR_ALL, "Enable hoedown extension Strikethrough"),
	AP_INIT_FLAG("HoedownExtUnderline", hoedown_set_extensions_underline, NULL, OR_ALL, "Enable hoedown extension Underline"),
	AP_INIT_FLAG("HoedownExtHighlight", hoedown_set_extensions_highlight, NULL, OR_ALL, "Enable hoedown extension Highlight"),
	AP_INIT_FLAG("HoedownExtQuote", hoedown_set_extensions_quote, NULL, OR_ALL, "Enable hoedown extension Quote"),
	AP_INIT_FLAG("HoedownExtSuperscript", hoedown_set_extensions_superscript, NULL, OR_ALL, "Enable hoedown extension Superscript"),
	AP_INIT_FLAG("HoedownExtLaxSpacing", hoedown_set_extensions_laxspacing, NULL, OR_ALL, "Enable hoedown extension Lax Spacing"),
	AP_INIT_FLAG("HoedownExtNoIntraEmphasis", hoedown_set_extensions_nointraemphasis, NULL, OR_ALL, "Enable hoedown extension NoIntraEmphasis"),
	AP_INIT_FLAG("HoedownExtDisableIndentedCode", hoedown_set_extensions_disableindentedcode, NULL, OR_ALL, "Disable hoedown extension Indented Code"),
	#ifdef HOEDOWN_VERSION_EXTRAS
	AP_INIT_FLAG("HoedownExtSpecialAttribute", hoedown_set_extensions_specialattribute, NULL, OR_ALL, "Enable hoedown extension Special Attribute"),
	#endif

	/* Html Render mode options */
	AP_INIT_FLAG("HoedownRenderSkipHtml", hoedown_set_render_skiphtml, NULL, OR_ALL, "Enable hoedown render Skip HTML"),
	AP_INIT_FLAG("HoedownRenderSkipStyle", hoedown_set_render_skipstyle, NULL, OR_ALL, "Enable hoedown render Skip Style"),
	AP_INIT_FLAG("HoedownRenderSkipImages", hoedown_set_render_skipimages, NULL, OR_ALL, "Enable hoedown render Skip Images"),
	AP_INIT_FLAG("HoedownRenderSkipLinks", hoedown_set_render_skiplinks, NULL, OR_ALL, "Enable hoedown render Skip Links"),
	AP_INIT_FLAG("HoedownRenderExpandTabs", hoedown_set_render_expandtabs, NULL, OR_ALL, "Enable hoedown render Expand Tabs"),
	AP_INIT_FLAG("HoedownRenderSafelink", hoedown_set_render_safelink, NULL, OR_ALL, "Enable hoedown render Safelink"),
	AP_INIT_FLAG("HoedownRenderToc", hoedown_set_render_toc, NULL, OR_ALL, "Enable hoedown render Toc"),
	AP_INIT_FLAG("HoedownRenderHardWrap", hoedown_set_render_hardwrap, NULL, OR_ALL, "Enable hoedown render Hard Wrap"),
	AP_INIT_FLAG("HoedownRenderUseXhtml", hoedown_set_render_usexhtml, NULL, OR_ALL, "Enable hoedown render Use XHTML"),
	AP_INIT_FLAG("HoedownRenderEscape", hoedown_set_render_escape, NULL, OR_ALL, "Enable hoedown render Escape"),
	#ifdef HOEDOWN_VERSION_EXTRAS
	AP_INIT_FLAG("HoedownRenderUseTaskList", hoedown_set_render_usetasklist, NULL, OR_ALL, "Enable hoedown render Use Task List"),
	AP_INIT_FLAG("HoedownRenderLineContinue", hoedown_set_render_linecontinue, NULL, OR_ALL, "Enable hoedown render Line Continue"),
	#endif
	{NULL}
};

/* The handler function for our module.
 * This is where all the fun happens!
 */

apr_table_t* example_parse_args(request_rec* r) {

	apr_table_t* GET;
	apr_array_header_t* POST;

	ap_args_to_table(r, &GET);
	ap_parse_form_data(r, NULL, &POST, -1, 8192);
}

const char* example_get_value(apr_table_t* table, const char* key) {
	const apr_array_header_t* fields;
	int                         i;
	//apr_table_elts

}

/********************************************************
*	HOEDOWN HANDLER										*
********************************************************/
static int hoedown_handler(request_rec* r) {
	int ret = -1;
	int directory = 1;
	apr_file_t* fp = NULL;
	char* style = NULL;
	char* url = NULL;
	char* text = NULL;
	char* raw = NULL;
	char* toc = NULL;
	int toc_begin = HOEDOWN_TOC_BEGIN, toc_end = HOEDOWN_TOC_END;
	apreq_handle_t* apreq;
	apr_table_t* params;

	// hoedown: markdown
	size_t iunit = HOEDOWN_READ_UNIT, ounit = HOEDOWN_OUTPUT_UNIT;
	hoedown_buffer* ib, * ob;
	hoedown_document* markdown;
	hoedown_renderer* renderer;
	hoedown_html_renderer_state* state;

	if(strcmp(r->handler, "hoedown-handler")) {
		return DECLINED;
	}

	if(r->header_only) {
		return OK;
	}

	// config
	hoedown_config* cfg = ap_get_module_config(r->per_dir_config, &hoedown_module);

	// set contest type
	r->content_type = HOEDOWN_CONTENT_TYPE;

	// get parameter
	apreq = apreq_handle_apache2(r);
	params = apreq_params(apreq, r->pool);
	if(params) {
		style = (char*)apreq_params_as_string(r->pool, params, "style", APREQ_JOIN_AS_IS);
		#ifdef HOEDOWN_URL_SUPPORT
		url = (char*)apreq_params_as_string(r->pool, params, "url", APREQ_JOIN_AS_IS);
		#endif
		if(cfg->raw != 0) {
			raw = (char*)apr_table_get(params, "raw");
		}
		if(cfg->html & HOEDOWN_HTML_TOC) {
			toc = (char*)apr_table_get(params, "toc");
		}
		if(r->method_number == M_POST) {
			text = (char*)apreq_params_as_string(r->pool, params, "markdown", APREQ_JOIN_AS_IS);
		}
	}

	// reading everything
	ib = hoedown_buffer_new(iunit);
	if(!ib) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_CRIT, 0, r, "%s: Couldn't allocate input buffer.", MODULE_NAME, MODULE_HANDLER_TITLE);
	}
	hoedown_buffer_grow(ib, HOEDOWN_READ_UNIT);

	// page
	if(url || text) {
		directory = 0;
	}
	append_page_data(r, cfg, ib, r->filename, directory);

	// text
	if(text && strlen(text) > 0) {
		append_data(ib, text, strlen(text));
	}

	#ifdef HOEDOWN_URL_SUPPORT
	// url
	if(url && strlen(url) > 0) {
		CURL* curl;

		curl = curl_easy_init();
		if(!curl) {
			return HTTP_INTERNAL_SERVER_ERROR;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url);

		// curl
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)ib);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_url_data);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, HOEDOWN_CURL_TIMEOUT);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

		ret = curl_easy_perform(curl);

		curl_easy_cleanup(curl);
	}
	#endif

	// default page
	if(ib->size == 0) {
		ret = append_page_data(r, cfg, ib, NULL, 0);
		if(ret != APR_SUCCESS) {
			hoedown_buffer_free(ib);
			return ret;
		}
	}

	// default toc level
	toc_begin = cfg->toc.begin;
	toc_end = cfg->toc.end;

	if(ib->size > 0) {
		if(cfg->raw != 0 && raw != NULL) {
			r->content_type = "text/plain";
			ap_rwrite(ib->data, ib->size, r);
			hoedown_buffer_free(ib);
			return OK;
		}

		// output style header
		fp = style_header(r, cfg, style, r->filename);

		// performing markdown parsing
		ob = hoedown_buffer_new(HOEDOWN_OUTPUT_UNIT);

		// toc
		if(cfg->html & HOEDOWN_HTML_TOC) {
			if(toc) {
				size_t len = strlen(toc);
				int n;

				if(len > 0) {
					char* delim, * toc_b = NULL, * toc_e = NULL;
					delim = strstr(toc, ":");
					if(delim) {
						int i = delim - toc;
						toc_b = apr_pstrndup(r->pool, toc, i++);
						n = atoi(toc_b);
						if(n) {
							toc_begin = n;
						}

						toc_e = apr_pstrndup(r->pool, toc + i, len - i);
						n = atoi(toc_e);
						if(n) {
							toc_end = n;
						}
					} else {
						n = atoi(toc);
						if(n) {
							toc_begin = n;
						}
					}
				}
			}

			renderer = hoedown_html_toc_renderer_new(0);
			state = (hoedown_html_renderer_state*)renderer->opaque;

			state->flags = cfg->html;
			state->toc_data.level_offset = toc_begin;
			state->toc_data.nesting_level = toc_end;
			#ifdef HOEDOWN_VERSION_EXTRAS
			state->toc_data.header = cfg->toc.header;
			state->toc_data.footer = cfg->toc.footer;
			state->toc_data.unescape = cfg->toc.unescape;
			#endif

			markdown = hoedown_document_new(renderer, cfg->extensions, 16);

			hoedown_document_render(markdown, ob, ib->data, ib->size);

			hoedown_document_free(markdown);
			hoedown_html_renderer_free(renderer);

			ap_rwrite(ob->data, ob->size, r);

			hoedown_buffer_reset(ob);
		}

		// markdown render
		renderer = hoedown_html_renderer_new(cfg->html, toc_end);

		#ifdef HOEDOWN_VERSION_EXTRAS
		state = (hoedown_html_renderer_state*)renderer->opaque;
		if((state->flags & HOEDOWN_HTML_USE_TASK_LIST) && cfg->class.task) {
			state->class_data.task = cfg->class.task;
		}
		if(cfg->class.ol) {
			state->class_data.ol = cfg->class.ol;
		}
		if(cfg->class.ul) {
			state->class_data.ul = cfg->class.ul;
		}
		#endif

		markdown = hoedown_document_new(renderer, cfg->extensions, 16);

		hoedown_document_render(markdown, ob, ib->data, ib->size);

		hoedown_document_free(markdown);
		hoedown_html_renderer_free(renderer);

		// writing the result
		ap_rwrite(ob->data, ob->size, r);

		// cleanup
		hoedown_buffer_free(ob);
	} else {
		// output style header
		fp = style_header(r, cfg, style, r->filename);
	}

	// cleanup
	hoedown_buffer_free(ib);

	// output style footer
	style_footer(r, fp);

	return OK;
}

static void* hoedown_create_dir_config(apr_pool_t* p, char* UNUSED(dir)) {
	hoedown_config* cfg;

	cfg = apr_pcalloc(p, sizeof(hoedown_config));

	memset(cfg, 0, sizeof(hoedown_config));

	cfg->default_page = NULL;
	cfg->directory_index = HOEDOWN_DIRECTORY_INDEX;
	cfg->style.path = NULL;
	cfg->style.name = NULL;
	cfg->style.ext = HOEDOWN_STYLE_EXT;
	cfg->class.ul = NULL;
	cfg->class.ol = NULL;
	cfg->class.task = NULL;
	cfg->toc.begin = HOEDOWN_TOC_BEGIN;
	cfg->toc.end = HOEDOWN_TOC_END;
	cfg->toc.header = NULL;
	cfg->toc.footer = NULL;
	cfg->toc.unescape = 0;
	cfg->raw = 0;
	cfg->html = 0;
	cfg->extensions =
		HOEDOWN_EXT_TABLES | HOEDOWN_EXT_FENCED_CODE |
		HOEDOWN_EXT_AUTOLINK | HOEDOWN_EXT_STRIKETHROUGH |
		HOEDOWN_EXT_NO_INTRA_EMPHASIS;

	return (void*)cfg;
}

static void* hoedown_merge_dir_config(apr_pool_t* p, void* base_conf, void* override_conf) {
	hoedown_config* cfg = apr_pcalloc(p, sizeof(hoedown_config));
	hoedown_config* base = (hoedown_config*)base_conf;
	hoedown_config* override = (hoedown_config*)override_conf;

	if(override->default_page && strlen(override->default_page) > 0) {
		cfg->default_page = override->default_page;
	} else {
		cfg->default_page = base->default_page;
	}
	if(strcmp(override->directory_index, HOEDOWN_DIRECTORY_INDEX) != 0) {
		cfg->directory_index = override->directory_index;
	} else {
		cfg->directory_index = base->directory_index;
	}

	if(override->style.path && strlen(override->style.path) > 0) {
		cfg->style.path = override->style.path;
	} else {
		cfg->style.path = base->style.path;
	}
	if(override->style.name && strlen(override->style.name) > 0) {
		cfg->style.name = override->style.name;
	} else {
		cfg->style.name = base->style.name;
	}
	if(strcmp(override->style.ext, HOEDOWN_STYLE_EXT) != 0) {
		cfg->style.ext = override->style.ext;
	} else {
		cfg->style.ext = base->style.ext;
	}

	#ifdef HOEDOWN_VERSION_EXTRAS
	if(override->class.ul && strlen(override->class.ul) > 0) {
		cfg->class.ul = override->class.ul;
	} else {
		cfg->class.ul = base->class.ul;
	}
	if(override->class.ol && strlen(override->class.ol) > 0) {
		cfg->class.ol = override->class.ol;
	} else {
		cfg->class.ol = base->class.ol;
	}
	if(override->class.task && strlen(override->class.task) > 0) {
		cfg->class.task = override->class.task;
	} else {
		cfg->class.task = base->class.task;
	}
	#endif

	if(override->toc.begin != HOEDOWN_TOC_BEGIN) {
		cfg->toc.begin = override->toc.begin;
	} else {
		cfg->toc.begin = base->toc.begin;
	}
	if(override->toc.end != HOEDOWN_TOC_END) {
		cfg->toc.end = override->toc.end;
	} else {
		cfg->toc.end = base->toc.end;
	}
	#ifdef HOEDOWN_VERSION_EXTRAS
	if(override->toc.header && strlen(override->toc.header) > 0) {
		cfg->toc.header = override->toc.header;
	} else {
		cfg->toc.header = base->toc.header;
	}
	if(override->toc.footer && strlen(override->toc.footer) > 0) {
		cfg->toc.footer = override->toc.footer;
	} else {
		cfg->toc.footer = base->toc.footer;
	}
	if(override->toc.unescape) {
		cfg->toc.unescape = override->toc.unescape;
	} else {
		cfg->toc.unescape = base->toc.unescape;
	}
	#endif

	if(override->raw != 0) {
		cfg->raw = 1;
	} else {
		cfg->raw = base->raw;
	}

	if(override->extensions > 0) {
		cfg->extensions = override->extensions;
	} else {
		cfg->extensions = base->extensions;
	}
	if(override->html > 0) {
		cfg->html = override->html;
	} else {
		cfg->html = base->html;
	}

	return (void*)cfg;
}


/********************************************************
*	ADD HOEDOWN VERSION TO SERVER SIGNATURE				*
********************************************************/
static int hoedown_post_config(apr_pool_t* pconf, apr_pool_t* plog, apr_pool_t* ptemp, server_rec* global_server) {
	/* Define a temporary variable */
	char ver[100];

	/* Copy the first string into the variable */
	strcpy(ver, MODULE_NAME);
	strcat(ver, "/");
	strcat(ver, HOEDOWN_VERSION);

	ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, global_server, "%s: Module started %s...", MODULE_NAME, MODULE_HANDLER_TITLE);
	//ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, global_server, "%s: Sucessfully created Host for mapped to", MODULE_NAME);
	// Decorate the server string
	ap_add_version_component(pconf, ver);
	return OK;
}


/********************************************************
*	HOOK REGISTRATION									*
********************************************************/
static void hoedown_register_hooks(apr_pool_t* pool) {
	/* Hook the request handler */
	ap_hook_post_config(hoedown_post_config, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_handler(hoedown_handler, NULL, NULL, APR_HOOK_LAST);
}


/********************************************************
*	MODULE NAME TAG										*
********************************************************/
module AP_MODULE_DECLARE_DATA hoedown_module = {
	STANDARD20_MODULE_STUFF,
	hoedown_create_dir_config,            // Per-directory configuration handler
	hoedown_merge_dir_config,            // Merge handler for per-directory configurations
	NULL,            // Per-server configuration handler
	NULL,            // Merge handler for per-server configurations
	hoedown_directives,            // Any directives we may have for httpd
	hoedown_register_hooks   // Our hook registering function
};