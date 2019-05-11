#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdlib.h>
#include <string.h>
#include "imf.h"

static int has_key(xmlNode *n, const char *key) {
  return !strcmp(n->name, key);
}

static const char *get_text(xmlNode *n) {
  return XML_GET_CONTENT(n->children);
}

static int get_int(xmlNode *n) {
  const char *t = get_text(n);
  return atoi(t);
}

static am_chunk_t* am_chunk_from_xml_node(xmlNode *node) {
  if (!node) {
    return NULL;
  }

  am_chunk_t *res = (am_chunk_t*)malloc(sizeof(am_chunk_t));
  memset(res, 0, sizeof(am_chunk_t));

  for (xmlNode *el = node->children; el != NULL; el = el->next) {
    if (el->type == XML_ELEMENT_NODE) {
      if (has_key(el, "Path")) {
        strcpy(res->path, get_text(el));
      }
      else if (has_key(el, "VolumeIndex")) {
        res->volume_index = get_int(el);
      }
      else if (has_key(el, "Offset")) {
        res->offset = get_int(el);
      }
      else if (has_key(el, "Length")) {
        res->length = get_int(el);
      }
      else {
        fprintf(stderr, "unknown key %s\n", el->name);
      }
    }
  }
  
  return res;
}

static cpl_resource_t* cpl_resource_from_xml_node(xmlNode *node) {
  if (!node) {
    return NULL;
  }

  cpl_resource_t *res = (cpl_resource_t*)malloc(sizeof(cpl_resource_t));
  memset(res, 0, sizeof(cpl_resource_t));

  for (xmlNode *el = node->children; el != NULL; el = el->next) {
    if (el->type == XML_ELEMENT_NODE) {
      if (has_key(el, "Id")) {
        strcpy(res->id, get_text(el));
      }
      else if (has_key(el, "EditRate")) {
      }
      else if (has_key(el, "IntrinsicDuration")) {
        res->intrinsic_duration = get_int(el);
      }
      else if (has_key(el, "EntryPoint")) {
        res->entry_point = get_int(el);
      }
      else if (has_key(el, "SourceDuration")) {
        res->source_duration = get_int(el);
      }
      else if (has_key(el, "RepeatCount")) {
        res->repeat_count = get_int(el);
      }
      else if (has_key(el, "TrackFileId")) {
        strcpy(res->track_file_id, get_text(el));
      }
      else if (has_key(el, "SourceEncoding")) {
        strcpy(res->source_encoding, get_text(el));
      }
      else {
        fprintf(stderr, "unknown key %s\n", el->name);
      }
    }
  }
  
  return res;
}

static xmlXPathContextPtr setup_xpath_context(xmlDoc *cplDoc) {
  xmlXPathContextPtr ctx = xmlXPathNewContext(cplDoc);

  xmlXPathRegisterNs(ctx, BAD_CAST "cpl", BAD_CAST "http://www.smpte-ra.org/schemas/2067-3/2016");
  xmlXPathRegisterNs(ctx, BAD_CAST "cc", BAD_CAST "http://www.smpte-ra.org/schemas/2067-2/2016");

  return ctx;
}

typedef void* (*collect_func_t)(xmlNode *node);

linked_list_t* collect_xpath_results(const char* filename, const char *xpath, collect_func_t collect_func) {
  xmlInitParser();

  LIBXML_TEST_VERSION
  xmlDoc *cplDoc = xmlReadFile(filename, NULL, 0);
  if (!cplDoc) {
    xmlFreeDoc(cplDoc);
    xmlCleanupParser();
    return NULL;
  }

  linked_list_t *resources = NULL;
  xmlXPathContextPtr ctx = setup_xpath_context(cplDoc);
  
  xmlXPathObjectPtr search_res = xmlXPathEvalExpression(xpath, ctx);
  if (!xmlXPathNodeSetIsEmpty(search_res->nodesetval)) {
    xmlNodeSetPtr nodes = search_res->nodesetval; 
    for (int i = 0; i < nodes->nodeNr; ++i) {
      if (!nodes->nodeTab[i]) {
        fprintf(stderr, "error getting node at index %d\n", i);
        continue;
      }
      if (nodes->nodeTab[i]->type == XML_ELEMENT_NODE) {
        xmlNode *resource_node = nodes->nodeTab[i];
        if (resource_node) {
          void *res = collect_func(resource_node);
          resources = ll_append(resources, res);
        }
      }
    }
  } else {
    fprintf(stderr, "empty res for %s\n", xpath);
  }

  xmlXPathFreeObject(search_res);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(cplDoc);
  xmlCleanupParser();

  return resources;
}

linked_list_t* cpl_get_video_resources(const char* filename) {
  return collect_xpath_results(filename, "//cc:MainImageSequence//cpl:Resource", (collect_func_t)cpl_resource_from_xml_node);
}

linked_list_t* cpl_get_audio_resources(const char* filename) {
  return collect_xpath_results(filename, "//cc:MainAudioSequence//cpl:Resource", (collect_func_t)cpl_resource_from_xml_node);
}

extern void cpl_free_resources(linked_list_t *ll) {
  linked_list_t *head;
  while (head = ll_poph(&ll)) {
    void *res = head->user_data;
    free(res);
    free(head);
  }
}

am_chunk_t* am_get_chunk_for_resource(const char *filename, cpl_resource_t *resource) {

  // need to use local names because of namespace shizzle
  const char *query_s = "//*[local-name()='Asset'][*[local-name()='Id']='";
  const char *query_e = "']/*[local-name()='ChunkList']/*[local-name()='Chunk']";

  char xpath[1024];
  strcpy(xpath, query_s);
  strcat(xpath, resource->track_file_id);
  strcat(xpath, query_e);

  linked_list_t *ll = collect_xpath_results(filename, xpath, (collect_func_t)am_chunk_from_xml_node);
  if (!ll) {
    return NULL;
  }
  void *user_data = ll->user_data;
  free(ll);
  return user_data;
}

void am_free_chunk(am_chunk_t *chunk) {
  if (chunk) {
    free(chunk);
  }
}

