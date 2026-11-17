#include "config_json.h"
#include "preset_data.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

size_t doc2struct_preset_count(void) { return sizeof(embedded_presets)/sizeof(embedded_presets[0]); }
const char *doc2struct_preset_namespace(size_t i) { return i<doc2struct_preset_count()?embedded_presets[i].name:NULL; }
Str doc2struct_preset_pack(size_t i) {
    return i<doc2struct_preset_count()?(Str){(const char*)embedded_presets[i].data,embedded_presets[i].size}:(Str){0};
}
Str doc2struct_defaults_json(void) { return (Str){(const char*)preset_bytes_0,sizeof(preset_bytes_0)-1}; }
static int bad(Arena *a,const char *message) {
    if(!a->error[0])snprintf(a->error,sizeof(a->error),"%s",message);
    return 0;
}
static void copy_error(doc2struct_error *e,const Arena *a) {
    if(e)snprintf(e->message,sizeof(e->message),"%s",a->error[0]?a->error:"invalid rules");
}
static int same_case(Str a,Str b) {
    size_t i;if(a.n!=b.n)return 0;
    for(i=0;i<a.n;i++) {
        unsigned x=(unsigned char)a.p[i],y=(unsigned char)b.p[i];
        if(x>='a'&&x<='z')x-=32;
        if(y>='a'&&y<='z')y-=32;
        if(x!=y)return 0;
    }
    return 1;
}
static JValue *resolved(Arena *a,Str input,int preset) {
    JValue *base=ds_config_parse(a,doc2struct_defaults_json()),*extra=NULL,*name,*default_patterns;
    if(!base)return NULL;
    name=ds_config_get(base,"name");
    default_patterns=ds_config_get(ds_config_get(base,"remove"),"patterns");
    if(preset) {
        const char *colon;size_t i;Str ns,kind;JValue *pack,*types,*sets,*entry;
        input=ds_strip(input,0);
        if(!input.n)return base;
        colon=memchr(input.p,':',input.n);
        if(!colon){bad(a,"preset must be NAMESPACE:DOCUMENT_TYPE");return NULL;}
        ns=ds_strip((Str){input.p,(size_t)(colon-input.p)},0);
        kind=ds_strip((Str){colon+1,input.n-(size_t)(colon-input.p)-1},0);
        for(i=0;i<doc2struct_preset_count();i++)if(same_case(ns,ds_str(embedded_presets[i].name)))break;
        if(i==doc2struct_preset_count()){bad(a,"unknown preset namespace");return NULL;}
        pack=ds_config_parse(a,doc2struct_preset_pack(i));if(!pack)return NULL;
        types=ds_config_get(pack,"document_types");sets=ds_config_get(pack,"rule_sets");
        if(!types||!sets){bad(a,"invalid embedded preset pack");return NULL;}
        for(entry=types->child;entry;entry=entry->next)if(same_case(entry->key,kind))break;
        if(!entry){bad(a,"unknown preset document type");return NULL;}
        for(extra=sets->child;extra;extra=extra->next)if(ds_eq(extra->key,entry->text))break;
        if(!extra){bad(a,"preset refers to missing rule set");return NULL;}
        ds_config_merge(a,base,extra);
        name=ds_config_get(base,"name");
        name->text=ds_cat(a,ds_cat(a,ds_str(embedded_presets[i].name),ds_str(":")),kind);
        if(name->text.p) {
            char *p=(char*)name->text.p;
            for(i=0;i<name->text.n;i++)if(p[i]>='a'&&p[i]<='z')p[i]-=32;
        }
    } else {
        extra=ds_config_parse(a,input);
        if(!extra||extra->type!=J_OBJECT){bad(a,"rules must be a JSON object");return NULL;}
        name->text=ds_str("custom");
        ds_config_merge(a,base,extra);
    }
    /* Preserve the existing explicit-null spelling for default remove patterns. */
    {
        JValue *patterns=ds_config_get(ds_config_get(base,"remove"),"patterns");
        if(patterns&&patterns->type==J_NULL) {
            JValue *next=patterns->next;*patterns=*default_patterns;patterns->next=next;
        }
    }
    return a->error[0]?NULL:base;
}
static JValue *field(Arena *a,JValue *object,const char *key,int type,int optional) {
    JValue *v=ds_config_get(object,key);
    if(!v&&optional)return NULL;
    if(!v||v->type!=type) {
        if(!a->error[0])snprintf(a->error,sizeof(a->error),"invalid rules field '%s'",key);
        return NULL;
    }
    return v;
}
static Str text(Rules *r,JValue *v) {
    if(!v||v->type!=J_STRING){bad(&r->arena,"expected a rules string");return(Str){0};}
    return ds_copy(&r->arena,v->text);
}
static long number(Rules *r,JValue *v) {
    char *end;long n;
    if(!v||v->type!=J_NUMBER){bad(&r->arena,"expected an integer rule value");return 0;}
    errno=0;n=strtol(v->text.p,&end,10);
    if(errno==ERANGE||end!=v->text.p+v->text.n)bad(&r->arena,"rule integer is outside the native range");
    return n;
}
static void *array(Rules *r,size_t n,size_t width) {
    if(n>SIZE_MAX/width){bad(&r->arena,"rules array too large");return NULL;}
    return ds_alloc(&r->arena,n*width);
}
static int one_of(Str s,const char *const *allowed) {
    size_t i;for(i=0;allowed[i];i++)if(ds_is(s,allowed[i]))return 1;return 0;
}
static Rules *compile(JValue *root,doc2struct_error *error) {
    static const char *const sources[]={"text","header_title","href","src","font_size","indent","flags",NULL};
    static const char *const dtypes[]={"str","int","bool","list",NULL};
    static const char *const which[]={"first","last","all",NULL};
    static const char *const reserved[]={"id","level","type","text","matrix","cleaned","preamble","footnotes","postamble","runs","cell_runs",NULL};
    static const char *const adapters[]={"html","text","partialsgml","pdf",NULL};
    static const char *const html_fields[]={"hide_hidden_attr","hide_visibility","max_span",NULL};
    Rules *r=calloc(1,sizeof(*r));JValue *v,*c,*sub,*x,*adapter;size_t i,j;long n;
    if(!r){if(error)snprintf(error->message,sizeof(error->message),"out of memory");return NULL;}
    if(!root||root->type!=J_OBJECT){bad(&r->arena,"rules must be an object");goto fail;}
    if(!field(&r->arena,root,"name",J_STRING,0))goto fail;
    if(ds_config_get(root,"options")||ds_config_get(root,"tab_size")) {
        bad(&r->arena,"input settings belong under adapters.html, adapters.text or adapters.partialsgml");goto fail;
    }
    sub=field(&r->arena,root,"adapters",J_OBJECT,0);if(!sub)goto fail;
    for(c=sub->child;c;c=c->next) {
        if(!one_of(c->key,adapters)){bad(&r->arena,"unknown adapter");goto fail;}
    }
    adapter=field(&r->arena,sub,"html",J_OBJECT,0);if(!adapter)goto fail;
    for(c=adapter->child;c;c=c->next) {
        if(!one_of(c->key,html_fields)){bad(&r->arena,"unknown HTML adapter setting");goto fail;}
    }
    x=field(&r->arena,adapter,"hide_hidden_attr",J_BOOL,0);if(!x)goto fail;r->html.hide_hidden_attr=x->boolean;
    x=field(&r->arena,adapter,"hide_visibility",J_BOOL,0);if(!x)goto fail;r->html.hide_visibility=x->boolean;
    n=number(r,field(&r->arena,adapter,"max_span",J_NUMBER,0));
    if(n<1){bad(&r->arena,"adapters.html.max_span must be positive");goto fail;}
    r->html.max_span=(size_t)n;
    for(i=1;i<3;i++) {
        adapter=field(&r->arena,sub,adapters[i],J_OBJECT,0);if(!adapter)goto fail;
        for(c=adapter->child;c;c=c->next) {
            if(!ds_is(c->key,"tab_size")){bad(&r->arena,"unknown text adapter setting");goto fail;}
        }
        n=number(r,field(&r->arena,adapter,"tab_size",J_NUMBER,0));
        if(n<1||n>256){bad(&r->arena,"adapter tab_size must be between 1 and 256");goto fail;}
        if(i==1)r->text_tab_size=(size_t)n;else r->partialsgml_tab_size=(size_t)n;
    }
    adapter=field(&r->arena,sub,"pdf",J_OBJECT,1);
    if(adapter) {
        static const char *const pdf_fields[]={"clip_to_page","detect_decorations","layout","font_recovery",NULL};
        for(c=adapter->child;c;c=c->next)
            if(!one_of(c->key,pdf_fields)){bad(&r->arena,"unknown PDF adapter setting");goto fail;}
        x=field(&r->arena,adapter,"clip_to_page",J_BOOL,1);if(x)r->pdf.clip_to_page=x->boolean;
        x=field(&r->arena,adapter,"detect_decorations",J_BOOL,1);if(x)r->pdf.detect_decorations=x->boolean;
        x=field(&r->arena,adapter,"layout",J_STRING,1);
        if(x) {
            if(!ds_is(x->text,"default")&&!ds_is(x->text,"regions")){bad(&r->arena,"PDF layout must be default or regions");goto fail;}
            r->pdf.region_layout=ds_is(x->text,"regions");
        }
        x=field(&r->arena,adapter,"font_recovery",J_STRING,1);
        if(x) {
            if(!ds_is(x->text,"default")&&!ds_is(x->text,"metadata")&&!ds_is(x->text,"glyphs")){bad(&r->arena,"PDF font_recovery must be default, metadata or glyphs");goto fail;}
            r->pdf.font_recovery=ds_is(x->text,"metadata")?1:ds_is(x->text,"glyphs")?2:0;
        }
    }
    if(r->arena.error[0])goto fail;
    v=field(&r->arena,root,"header_styles",J_ARRAY,0);if(!v)goto fail;
    for(c=v->child;c;c=c->next) {
        unsigned bit=ds_style_bit(text(r,c));
        if(!bit){bad(&r->arena,"unknown default header style");goto fail;}
        r->header_styles|=bit;
    }
    v=field(&r->arena,root,"headers",J_ARRAY,0);if(!v)goto fail;
    r->nheaders=v->count;r->headers=array(r,v->count,sizeof(Header));if(!r->headers)goto fail;
    for(c=v->child,i=0;c;c=c->next,i++) {
        Header *h=r->headers+i;long level;
        if(c->type!=J_OBJECT){bad(&r->arena,"header must be an object");goto fail;}
        h->name=text(r,field(&r->arena,c,"name",J_STRING,0));
        level=number(r,field(&r->arena,c,"level",J_NUMBER,0));
        if(level<INT_MIN||level>=INT_MAX){bad(&r->arena,"header level is outside the native integer range");goto fail;}
        h->level=(int)level;
        x=field(&r->arena,c,"regex",J_STRING,0);
        if(r->arena.error[0]||!x)goto fail;
        h->rx=ds_regex(r,x->text,0);if(!h->rx)goto fail;
        x=field(&r->arena,c,"requires",J_ARRAY,1);
        h->has_requires=x!=NULL;
        if(x)for(x=x->child;x;x=x->next) {
            unsigned bit=ds_style_bit(text(r,x));
            if(!bit){bad(&r->arena,"unknown required header style");goto fail;}
            h->requires|=bit;
        }
        if(r->arena.error[0])goto fail;
    }
    sub=field(&r->arena,root,"text",J_OBJECT,0);if(!sub)goto fail;
    v=field(&r->arena,sub,"preserve_styles",J_ARRAY,0);if(!v)goto fail;
    for(c=v->child;c;c=c->next) {
        unsigned bit=ds_style_bit(text(r,c));
        if(!bit||(bit&~(unsigned)TEXT_STYLES)){bad(&r->arena,"preserve_styles accepts bold, italic, underline and strikethrough");goto fail;}
        r->preserve_styles|=bit;
    }
    x=field(&r->arena,sub,"join_wrapped_lines",J_BOOL,0);if(!x)goto fail;r->join_wrapped_lines=x->boolean;
    x=field(&r->arena,sub,"wrap_hyphenation",J_STRING,0);if(!x)goto fail;
    if(!ds_is(x->text,"preserve")&&!ds_is(x->text,"remove")){bad(&r->arena,"wrap_hyphenation must be preserve or remove");goto fail;}
    r->remove_wrap_hyphen=ds_is(x->text,"remove");
    sub=field(&r->arena,root,"headings",J_OBJECT,0);if(!sub)goto fail;
    x=field(&r->arena,sub,"infer",J_BOOL,0);if(!x)goto fail;r->infer_headings=x->boolean;
    x=field(&r->arena,sub,"merge_adjacent",J_BOOL,0);if(!x)goto fail;r->merge_adjacent_headings=x->boolean;
    r->heading_max_words=number(r,field(&r->arena,sub,"max_words",J_NUMBER,0));
    if(r->heading_max_words<0){bad(&r->arena,"headings.max_words must be nonnegative");goto fail;}
    v=field(&r->arena,sub,"styles",J_ARRAY,0);if(!v)goto fail;
    for(c=v->child;c;c=c->next) {
        unsigned bit=ds_style_bit(text(r,c));
        if(!bit||bit==64){bad(&r->arena,"invalid inferred heading style");goto fail;}
        r->heading_styles|=bit;
    }
    sub=field(&r->arena,root,"remove",J_OBJECT,0);if(!sub)goto fail;
    x=ds_config_get(sub,"repeat_threshold");
    r->repeat_threshold=x&&x->type==J_NULL?0:number(r,x);
    x=ds_config_get(sub,"page_label_regex");
    if(x&&x->type!=J_NULL&&x->type!=J_STRING){bad(&r->arena,"invalid page_label_regex");goto fail;}
    if(x&&x->type==J_STRING&&x->text.n){r->page_label=ds_regex(r,x->text,0);if(!r->page_label)goto fail;}
    v=field(&r->arena,sub,"patterns",J_ARRAY,0);if(!v)goto fail;
    r->nremove=v->count;r->remove=array(r,v->count,sizeof(Remove));if(!r->remove)goto fail;
    for(c=v->child,i=0;c;c=c->next,i++) {
        if(c->type!=J_ARRAY||c->count!=2||c->child->type!=J_STRING||c->child->next->type!=J_BOOL) {
            bad(&r->arena,"remove pattern must be [string, boolean]");goto fail;
        }
        r->remove[i].rx=ds_regex(r,c->child->text,0);
        r->remove[i].needs_href=c->child->next->boolean;
        if(!r->remove[i].rx)goto fail;
    }
    sub=field(&r->arena,root,"tables",J_OBJECT,0);if(!sub)goto fail;
#define BOOL(name) x=field(&r->arena,sub,#name,J_BOOL,0);if(!x)goto fail;r->name=x->boolean
#define NUMBER(name) r->name=number(r,field(&r->arena,sub,#name,J_NUMBER,0))
    BOOL(single_row_to_text);BOOL(title_from_section);BOOL(unwrap_layout);BOOL(protect_years);
    NUMBER(formatting_max_gap);
    if(r->formatting_max_gap < -1){bad(&r->arena,"formatting_max_gap must be -1 or nonnegative");goto fail;}
    NUMBER(footnote_lines);NUMBER(preamble_lines);NUMBER(postamble_lines);
#undef BOOL
#undef NUMBER
    x=ds_config_get(sub,"footnote_regex");
    if(!x||(x->type!=J_NULL&&x->type!=J_STRING)){bad(&r->arena,"invalid footnote_regex");goto fail;}
    if(x->type==J_STRING&&x->text.n){r->footnote=ds_regex(r,x->text,0);if(!r->footnote)goto fail;}
    x=ds_config_get(sub,"annotation_exclude_regex");
    if(x&&x->type!=J_NULL&&x->type!=J_STRING){bad(&r->arena,"invalid annotation_exclude_regex");goto fail;}
    if(x&&x->type==J_STRING&&x->text.n){r->annotation_exclude=ds_regex(r,x->text,0);if(!r->annotation_exclude)goto fail;}
    v=field(&r->arena,sub,"steps",J_ARRAY,0);if(!v)goto fail;
    for(c=v->child;c;c=c->next) {
        Str step=text(r,c);
        for(j=0;ds_table_steps[j];j++)if(ds_is(step,ds_table_steps[j]))break;
        if(!ds_table_steps[j]){bad(&r->arena,"unknown table step");goto fail;}
        r->table_steps|=1u<<j;
    }
    sub=field(&r->arena,root,"lists",J_OBJECT,0);if(!sub)goto fail;
    r->list_gap_rows=number(r,field(&r->arena,sub,"max_gap_rows",J_NUMBER,0));
    r->list_tail_rows=number(r,field(&r->arena,sub,"max_tail_rows",J_NUMBER,0));
    if(r->list_gap_rows<0||r->list_tail_rows<0){bad(&r->arena,"list gaps must be nonnegative");goto fail;}
    if(r->arena.error[0])goto fail;
    v=field(&r->arena,root,"columns",J_ARRAY,0);if(!v)goto fail;
    r->ncolumns=v->count;r->columns=array(r,v->count,sizeof(Column));if(!r->columns)goto fail;
    for(c=v->child,i=0;c;c=c->next,i++) {
        Column *col=r->columns+i;
        if(c->type!=J_OBJECT){bad(&r->arena,"column must be an object");goto fail;}
        col->name=text(r,field(&r->arena,c,"name",J_STRING,0));
        col->source=text(r,field(&r->arena,c,"source",J_STRING,0));
        x=field(&r->arena,c,"dtype",J_STRING,1);col->dtype=x?text(r,x):ds_str("str");
        x=field(&r->arena,c,"which",J_STRING,1);col->which=x?text(r,x):ds_str("first");
        if(one_of(col->name,reserved)){bad(&r->arena,"custom column reuses a default column name");goto fail;}
        for(j=0;j<i;j++)if(ds_eq(col->name,r->columns[j].name)){bad(&r->arena,"duplicate custom column name");goto fail;}
        if(!one_of(col->source,sources)||!one_of(col->dtype,dtypes)||!one_of(col->which,which)) {
            bad(&r->arena,"invalid custom column source, dtype or which");goto fail;
        }
        if(ds_is(col->source,"href"))r->collect_hrefs=1;
        x=ds_config_get(c,"pattern");
        if(x&&x->type!=J_NULL) {
            if(x->type!=J_STRING){bad(&r->arena,"column pattern must be a string");goto fail;}
            if(x->text.n){col->rx=ds_regex(r,x->text,1);if(!col->rx)goto fail;}
        }
        x=field(&r->arena,c,"value",J_STRING,1);
        if(x) {
            if(!ds_is(col->source,"text")&&!ds_is(col->source,"header_title")) {
                bad(&r->arena,"column value requires a text or header_title source");goto fail;
            }
            if(!col->rx){bad(&r->arena,"column value requires a nonempty pattern");goto fail;}
            col->value=text(r,x);
        }
        if(r->arena.error[0])goto fail;
        x=ds_config_get(c,"types");
        if(x&&x->type!=J_NULL) {
            if(x->type!=J_ARRAY){bad(&r->arena,"column types must be an array");goto fail;}
            col->ntypes=x->count;col->filter_types=x->count!=0;
            col->types=array(r,x->count,sizeof(Str));if(!col->types)goto fail;
            for(x=x->child,j=0;x;x=x->next,j++)col->types[j]=text(r,x);
        }
        x=ds_config_get(c,"classes");
        if(x&&x->type!=J_NULL) {
            if(x->type!=J_ARRAY){bad(&r->arena,"column classes must be an array");goto fail;}
            col->nclasses=x->count;col->filter_classes=x->count!=0;
            col->classes=array(r,x->count,sizeof(Str));if(!col->classes)goto fail;
            for(x=x->child,j=0;x;x=x->next,j++)col->classes[j]=text(r,x);
        }
    }
    if(r->arena.error[0])goto fail;
    return r;
fail:
    copy_error(error,&r->arena);ds_rules_free(r);free(r);return NULL;
}
static Rules *load(Str input,int preset,doc2struct_error *error) {
    Arena arena={0};JValue *root;Rules *r=NULL;
    if(error)error->message[0]=0;
    root=resolved(&arena,input,preset);
    if(root)r=compile(root,error);else copy_error(error,&arena);
    ds_arena_free(&arena);return r;
}
doc2struct_rules *doc2struct_rules_preset(const char *name,doc2struct_error *error) {
    return load(ds_str(name?name:""),1,error);
}
doc2struct_rules *doc2struct_rules_json(const char *json,size_t n,doc2struct_error *error) {
    return load((Str){json,n},0,error);
}
void doc2struct_rules_free(doc2struct_rules *r) { if(r){ds_rules_free(r);free(r);} }
const doc2struct_column *doc2struct_rules_columns(const doc2struct_rules *r,size_t *count) {
    if(count)*count=r?r->ncolumns:0;return r?r->columns:NULL;
}
char *doc2struct_rules_resolve(const char *input,size_t n,int preset,size_t *size,doc2struct_error *error) {
    Arena arena={0};JValue *root;Rules *r;Str json={0};char *out=NULL;
    if(size)*size=0;if(error)error->message[0]=0;
    root=resolved(&arena,(Str){input,n},preset);
    if(root) {
        r=compile(root,error);
        if(r){doc2struct_rules_free(r);json=ds_config_write(&arena,root);}
    }
    if(json.p){out=malloc(json.n+1);if(out){memcpy(out,json.p,json.n+1);if(size)*size=json.n;}else bad(&arena,"out of memory");}
    if(arena.error[0])copy_error(error,&arena);
    ds_arena_free(&arena);return out;
}
char *doc2struct_rules_merge_json(Str base_json,Str extra_json,size_t *size,doc2struct_error *error) {
    Arena a={0};JValue *base,*extra,*defaults,*b,*x,*tail,*adapter,*setting,*default_adapter;Rules *r=NULL;
    Str json={0};char *out=NULL;size_t i;
    static const char *const append[]={"headers","columns"};
    static const char *const replace[]={"remove","tables","lists","header_styles","text","headings"};
    if(size)*size=0;if(error)error->message[0]=0;
    base=resolved(&a,base_json,0);extra=resolved(&a,extra_json,0);
    defaults=ds_config_parse(&a,doc2struct_defaults_json());
    if(!base||!extra||!defaults)goto done;
    r=compile(base,error);if(!r)goto done;doc2struct_rules_free(r);
    r=compile(extra,error);if(!r)goto done;doc2struct_rules_free(r);
    /* Adapter overrides apply per setting, preserving the other adapters and
     * existing nondefault settings, as when adding just a different tab size. */
    for(adapter=ds_config_get(extra,"adapters")->child;adapter;adapter=adapter->next) {
        b=ds_config_get(ds_config_get(base,"adapters"),adapter->key.p);
        default_adapter=ds_config_get(ds_config_get(defaults,"adapters"),adapter->key.p);
        for(setting=adapter->child;setting;setting=setting->next) {
            x=ds_config_get(b,setting->key.p);
            if(!ds_config_equal(setting,ds_config_get(default_adapter,setting->key.p))) {
                JValue *next=x->next;*x=*setting;x->next=next;
            }
        }
    }
    for(i=0;i<2;i++) {
        b=ds_config_get(base,append[i]);x=ds_config_get(extra,append[i]);
        if(!b->child)b->child=x->child;
        else {for(tail=b->child;tail->next;tail=tail->next){}tail->next=x->child;}
        b->count+=x->count;
    }
    for(i=0;i<sizeof(replace)/sizeof(replace[0]);i++) {
        b=ds_config_get(base,replace[i]);x=ds_config_get(extra,replace[i]);
        if(!ds_config_equal(x,ds_config_get(defaults,replace[i]))) {
            JValue *next=b->next;*b=*x;b->next=next;
        }
    }
    b=ds_config_get(base,"name");x=ds_config_get(extra,"name");
    b->text=ds_cat(&a,ds_cat(&a,b->text,ds_str("+")),x->text);
    if(a.error[0])goto done;
    r=compile(base,error);if(!r)goto done;doc2struct_rules_free(r);
    json=ds_config_write(&a,base);
    if(json.p) {
        out=malloc(json.n+1);
        if(out){memcpy(out,json.p,json.n+1);if(size)*size=json.n;}
        else bad(&a,"out of memory");
    }
done:
    if(a.error[0])copy_error(error,&a);
    ds_arena_free(&a);return out;
}
