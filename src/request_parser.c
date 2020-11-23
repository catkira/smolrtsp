#include "parsing_aux.h"
#include <smolrtsp/request_parser.h>

#include <stdbool.h>
#include <stdlib.h>

struct SmolRTSP_RequestParser {
    SmolRTSP_ParseRequestResult state;
    SmolRTSP_Cursor cursor;
};

SmolRTSP_RequestParser *SmolRTSP_RequestParser_new(void) {
    SmolRTSP_RequestParser *parser;
    if ((parser = malloc(sizeof(*parser))) == NULL) {
        return NULL;
    }

    parser->state = SmolRTSP_ParseRequestResultNothingParsed;
    parser->cursor = NULL;

    return parser;
}

void SmolRTSP_RequestParser_free(SmolRTSP_RequestParser *parser) {
    free(parser);
}

SmolRTSP_ParseRequestResult SmolRTSP_RequestParser_parse(
    SmolRTSP_RequestParser *restrict parser, SmolRTSP_Request *restrict request, size_t size,
    const void *restrict data) {
    if (parser->state == SmolRTSP_ParseRequestResultOk ||
        parser->state == SmolRTSP_ParseRequestResultErr) {
        return parser->state;
    }

    parser->cursor = data;

    typedef SmolRTSP_DeserializeResult (*Deserializer)(
        void *restrict self, size_t size, const void *restrict data, size_t *restrict bytes_read);

    typedef struct {
        Deserializer deserializer;
        void *entity;
        SmolRTSP_ParseRequestResult next_state_if_ok;
    } FSMTransition;

#define ASSOC(state, type, entity)                                                                 \
    [SmolRTSP_ParseRequestResult##state] = {                                                       \
        (Deserializer)SmolRTSP_##type##_deserialize,                                               \
        entity,                                                                                    \
        .next_state_if_ok = SmolRTSP_ParseRequestResult##type##Parsed,                             \
    }

    const FSMTransition transition_table[] = {
        ASSOC(NothingParsed, Method, &request->start_line.method),
        ASSOC(MethodParsed, RequestURI, &request->start_line.uri),
        ASSOC(RequestURIParsed, RTSPVersion, &request->start_line.version),
        ASSOC(HeaderMapParsed, HeaderMap, &request->header_map),
    };

#undef ASSOC

    FSMTransition transition = transition_table[parser->state];

    while (true) {
        size_t bytes_read;
        SmolRTSP_DeserializeResult res =
            transition.deserializer(transition.entity, size, parser->cursor, &bytes_read);
        parser->cursor += bytes_read;
        size -= bytes_read;

        switch (res) {
        case SmolRTSP_DeserializeResultOk:
            if (parser->state == SmolRTSP_ParseRequestResultHeaderMapParsed) {
                parser->state = SmolRTSP_ParseRequestResultOk;
                return parser->state;
            }

            parser->state = transition.next_state_if_ok;
            break;
        case SmolRTSP_DeserializeResultErr:
            parser->state = SmolRTSP_ParseRequestResultErr;
            return parser->state;
        case SmolRTSP_DeserializeResultNeedMore:
            return parser->state;
        }
    }
}
