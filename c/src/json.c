#include "doc2struct.h"
#include "scan.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    Arena *arena;
    char *data;
    size_t size, capacity;
} Writer;

static int write_bytes(Writer *writer, const char *bytes, size_t size) {
    size_t needed, capacity;
    char *next;
    if (size > SIZE_MAX - writer->size - 1) {
        snprintf(writer->arena->error, sizeof(writer->arena->error), "JSON output too large");
        return 0;
    }
    needed = writer->size + size + 1;
    if (needed > writer->capacity) {
        capacity = writer->capacity ? writer->capacity : 4096;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        next = ds_alloc_uninit(writer->arena, capacity);
        if (!next) return 0;
        if (writer->size) memcpy(next, writer->data, writer->size);
        writer->data = next; writer->capacity = capacity;
    }
    if (size) memcpy(writer->data + writer->size, bytes, size);
    writer->size += size;
    return 1;
}
static int literal(Writer *writer, const char *bytes) { return write_bytes(writer,bytes,strlen(bytes)); }
static int string_value(Writer *writer, Str value) {
    static const char hex[] = "0123456789abcdef";
    size_t i = 0, plain;
    if (!value.p) return literal(writer,"null");
    if (!literal(writer,"\"")) return 0;
    while (i < value.n) {
        unsigned char c;
        const char *escape = NULL;
        plain=ds_json_escape(value.p+i,value.n-i);
        if (!write_bytes(writer,value.p+i,plain)) return 0;
        i+=plain;
        if (i==value.n) break;
        c=(unsigned char)value.p[i++];
        switch(c) {
        case '"':escape="\\\"";break;
        case '\\':escape="\\\\";break;
        case '\b':escape="\\b";break;
        case '\f':escape="\\f";break;
        case '\n':escape="\\n";break;
        case '\r':escape="\\r";break;
        case '\t':escape="\\t";break;
        }
        if(escape) { if(!write_bytes(writer,escape,2))return 0; }
        else { char unicode_escape[]={'\\','u','0','0',hex[c>>4],hex[c&15]};
            if(!write_bytes(writer,unicode_escape,6))return 0; }
    }
    return literal(writer,"\"");
}
static int integer_value(Writer *writer, size_t value) {
    char bytes[32];int size=snprintf(bytes,sizeof(bytes),"%zu",value);
    return size>0 && write_bytes(writer,bytes,(size_t)size);
}
static int matrix_value(Writer *writer,const Matrix *matrix) {
    size_t i,j;
    if(!matrix->nr)return literal(writer,"null");
    if(!literal(writer,"["))return 0;
    for(i=0;i<matrix->nr;i++) {
        if(i&&!literal(writer,","))return 0;
        if(!literal(writer,"["))return 0;
        for(j=0;j<matrix->nc;j++) {
            if(j&&!literal(writer,","))return 0;
            if(!string_value(writer,matrix->cells[i*matrix->nc+j]))return 0;
        }
        if(!literal(writer,"]"))return 0;
    }
    return literal(writer,"]");
}
static int notes_value(Writer *writer,const Row *row) {
    size_t i;
    if(!row->nnotes)return literal(writer,"null");
    if(!literal(writer,"["))return 0;
    for(i=0;i<row->nnotes;i++) {
        if(i&&!literal(writer,","))return 0;
        if(!literal(writer,"[")||!string_value(writer,row->notes[i].id)||
           !literal(writer,",")||!string_value(writer,row->notes[i].text)||!literal(writer,"]"))return 0;
    }
    return literal(writer,"]");
}
static int cleanup_value(Writer *writer,const Row *row) {
    size_t i;int comma=0;
    if(row->cleaned<0)return literal(writer,"null");
    if(!literal(writer,"["))return 0;
    for(i=0;ds_cleanup_names[i];i++)
        if(row->cleaned&(1u<<i)) {
            if(comma++&&!literal(writer,","))return 0;
            if(!string_value(writer,ds_str(ds_cleanup_names[i])))return 0;
        }
    return literal(writer,"]");
}
Str ds_json(Document *document,const Rules *rules,int with_rules,Arena *arena) {
    Writer writer={0};size_t i,j;writer.arena=arena;
    (void)with_rules; /* Output columns are declared in rules, independently of this legacy flag. */
    if(!literal(&writer,"["))return(Str){0};
    for(i=0;i<document->rows.n;i++) {
        const Row *row=(const Row*)document->rows.p+i;
        if(i&&!literal(&writer,","))return(Str){0};
        if(!literal(&writer,"{\"id\":")||!integer_value(&writer,row->id)||
           !literal(&writer,",\"level\":")||!integer_value(&writer,row->level)||
           !literal(&writer,",\"type\":")||!string_value(&writer,row->type))return(Str){0};
        if(!literal(&writer,",\"text\":")||!string_value(&writer,row->text)||
           !literal(&writer,",\"matrix\":")||!matrix_value(&writer,&row->matrix)||
           !literal(&writer,",\"cleaned\":")||!cleanup_value(&writer,row)||
           !literal(&writer,",\"preamble\":")||!string_value(&writer,row->preamble)||
           !literal(&writer,",\"footnotes\":")||!notes_value(&writer,row)||
           !literal(&writer,",\"postamble\":")||!string_value(&writer,row->postamble))return(Str){0};
        if(row->style_mask) {
            size_t k;
            if(!literal(&writer,",\"runs\":["))return(Str){0};
            for(k=0;k<row->ntext_runs;k++) {
                if(k&&!literal(&writer,","))return(Str){0};
                if(!literal(&writer,"{\"text\":")||!string_value(&writer,row->text_runs[k].text)||
                   !literal(&writer,",\"flags\":")||!integer_value(&writer,row->text_runs[k].flags)||!literal(&writer,"}"))return(Str){0};
            }
            if(!literal(&writer,"]"))return(Str){0};
        }
        for(j=0;j<rules->ncolumns;j++) {
            const Column *column=rules->columns+j;
            Str value={0};
            if(doc2struct_column_applies(column,row)&&
               !doc2struct_column_text(document,column,
                   ds_is(column->source,"header_title")?row->title:row->text,&value)) {
                snprintf(arena->error,sizeof(arena->error),"%s",doc2struct_document_error(document));
                return(Str){0};
            }
            if(!literal(&writer,",")||!string_value(&writer,column->name)||
               !literal(&writer,":")||!string_value(&writer,value))return(Str){0};
        }
        if(!literal(&writer,"}"))return(Str){0};
    }
    if(!literal(&writer,"]"))return(Str){0};
    writer.data[writer.size]=0;
    return(Str){writer.data,writer.size};
}
