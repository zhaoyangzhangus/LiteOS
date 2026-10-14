"""Production HTTP request construction, neutral identity and bounded headers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/wget/main.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
''' + source[source.index('#define WGET_URL_CAP'):source.index('typedef struct wget_stream')]
code += '\n'.join(function(s) for s in (
    'static size_t text_length(', 'static bool copy_text_cap(',
    'static bool append_char(', 'static bool append_text(',
    'static bool append_u32_decimal(', 'static bool append_u64_decimal(',
    'static bool request_append_host(', 'static bool header_value_safe(',
    'static char base64_digit(', 'static bool append_basic_auth(',
    'static bool build_http_request(')) + r'''
int main(void){
 wget_url_t u={.host="example.test",.path="/path?q=1",.port=80};
 wget_options_t o={0};char request[4096];size_t length=0;
 assert(build_http_request(&u,&o,0,request,sizeof(request),&length));
 assert(length==strlen(request));
 assert(strstr(request,"GET /path?q=1 HTTP/1.1\r\nHost: example.test\r\n"));
 assert(strstr(request,"User-Agent: wget/1.0\r\n"));
 assert(!strstr(request,"LiteOS")&&!strstr(request,"LITEOS"));
 strcpy(o.user_agent,"custom/2");
 assert(build_http_request(&u,&o,123,request,sizeof(request),&length));
 assert(strstr(request,"User-Agent: custom/2\r\n")&&strstr(request,"Range: bytes=123-\r\n"));
 strcpy(o.user_agent,"custom\r\nInjected: yes");
 assert(!build_http_request(&u,&o,0,request,sizeof(request),&length));
 strcpy(o.user_agent,"custom\nInjected: yes");
 assert(!build_http_request(&u,&o,0,request,sizeof(request),&length));
 o.user_agent[0]=0;u.https=true;u.port=443;
 o.username="user";o.password="pass";
 assert(build_http_request(&u,&o,0,request,sizeof(request),&length));
 assert(strstr(request,"Authorization: Basic dXNlcjpwYXNz\r\n"));
 assert(strstr(request,"Host: example.test\r\n"));
 u.port=8443;o.post_data_set=true;strcpy(o.post_data,"body\r\ntext");
 assert(build_http_request(&u,&o,0,request,sizeof(request),&length));
 assert(strstr(request,"POST /path?q=1 HTTP/1.1\r\n"));
 assert(strstr(request,"Host: example.test:8443\r\n"));
 assert(strstr(request,"Content-Length: 10\r\n")&&strstr(request,"\r\n\r\nbody\r\ntext"));
 o=(wget_options_t){0};strcpy(u.host,"host\r\nInjected: yes");
 assert(!build_http_request(&u,&o,0,request,sizeof(request),&length));
 strcpy(u.host,"example.test");strcpy(u.path,"/path\nInjected: yes");
 assert(!build_http_request(&u,&o,0,request,sizeof(request),&length));
 strcpy(u.path,"/path");o.extra_header_count=WGET_MAX_EXTRA_HEADERS+1;
 assert(!build_http_request(&u,&o,0,request,sizeof(request),&length));
 o.extra_header_count=0;
 for(size_t capacity=1;capacity<400;++capacity){
  char guarded[416];memset(guarded,77,sizeof(guarded));
  bool success=build_http_request(&u,&o,0,guarded+8,capacity,&length);
  for(size_t i=0;i<8;++i)assert(guarded[i]==77);
  for(size_t i=8+capacity;i<sizeof(guarded);++i)assert(guarded[i]==77);
  if(success)assert(length<capacity&&guarded[8+length]==0);
 }
 puts("HTTP request: neutral/custom identity, CR/LF rejection, auth/range/POST and 399 capacity guards passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wget-request-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
