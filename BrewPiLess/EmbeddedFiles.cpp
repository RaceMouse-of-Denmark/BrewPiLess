#include <Arduino.h>
#include <pgmspace.h>
#include "espconfig.h"

#ifndef NULL
#define NULL 0
#endif


typedef struct _EmbeddedFileMapEntry{
	const char *filename;
	const uint8_t *content;
    unsigned int size;	
	bool  gzipped;
} EmbeddedFileMapEntry;

#include "data_bwf_js.h"
const char file_bwf_js [] PROGMEM="/bwf.js";

#include "data_index_htm.h"
const char file_index_htm [] PROGMEM="/index.htm";

#include "data_setup_htm.h"
const char file_setup_htm [] PROGMEM="/setup.htm";

#include "data_testcmd_htm.h"
const char file_testcmd_htm [] PROGMEM="/testcmd.htm";

#include "data_nindex_htm.h"
const char file_lcd [] PROGMEM="/lcd";

#include "data_viewlog_htm.h"
const char file_viewlog_htm [] PROGMEM="/viewlog.htm";

EmbeddedFileMapEntry fileMaps[]={
{file_bwf_js,data_bwf_min_js_gz,data_bwf_min_js_gz_len,true},
{file_index_htm,data_nindex_htm_gz,data_nindex_htm_gz_len,true},
{file_lcd,data_lcd_min_htm_gz,data_lcd_min_htm_gz_len,true},
{file_setup_htm,data_setup_min_htm_gz,data_setup_min_htm_gz_len,true},
{file_testcmd_htm,(const uint8_t *)data_testcmd_htm,0,false},
{file_viewlog_htm,data_viewlog_min_htm_gz,data_viewlog_min_htm_gz_len,true}
};

const uint8_t* getEmbeddedFile(const char* filename,bool &gzip, unsigned int &size)
{
	for(int i=0;i<sizeof(fileMaps)/sizeof(EmbeddedFileMapEntry);i++)
	{
		if(strcmp_P(filename,fileMaps[i].filename) ==0){
		    gzip = fileMaps[i].gzipped;
		    size = fileMaps[i].size;
			return fileMaps[i].content;
		}
	}
	return NULL;
} 























































