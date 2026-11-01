"""Compile and drive real OtaFetch.cpp with clocked, nonblocking TLS-Client doubles."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SECURE = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace fixture {
struct Reply {std::string head, body;int end=-1, waits=0;bool connectOk=true,writeOk=true;};
extern std::vector<Reply> replies;
extern std::vector<std::string> requests, hosts;
extern size_t next;
extern unsigned long clock;
extern int watchdog, live, peak;
}
inline unsigned long millis(){return fixture::clock;}
inline void delay(unsigned long n){fixture::clock += n;}
struct FakeEsp {unsigned getFreeHeap(){return 46860;} unsigned getMaxAllocHeap(){return 27636;}};
inline FakeEsp ESP;
namespace freeink {
class SecureClient {
 fixture::Reply* reply=nullptr;size_t h=0,b=0;bool up=false;int waits=0;
public:
 SecureClient(){fixture::peak=std::max(fixture::peak,++fixture::live);}
 ~SecureClient(){stop();--fixture::live;}
 void setInsecure(){}
 int connect(const char* host,uint16_t){
  assert(fixture::next<fixture::replies.size());reply=&fixture::replies[fixture::next++];
  fixture::hosts.push_back(host);fixture::requests.emplace_back();up=reply->connectOk;waits=reply->waits;return up;
 }
 size_t write(const uint8_t* p,size_t n){
  if(!reply->writeOk)return 0;
  fixture::requests.back().append(reinterpret_cast<const char*>(p),n);return n;
 }
 int read(uint8_t* p,size_t n){
  assert(n>0 && n<=4096 && up);
  if(waits>0){--waits;return 0;}
  if(h<reply->head.size()){
   n=std::min(n,reply->head.size()-h);memcpy(p,reply->head.data()+h,n);h+=n;
   if(h==reply->head.size())waits=reply->waits;return n;
  }
  if(b<reply->body.size()){
   n=std::min(n,reply->body.size()-b);memcpy(p,reply->body.data()+b,n);b+=n;return n;
  }
  if(reply->end==-2){up=false;return 0;}
  if(reply->end==-1){up=false;return -1;}
  return 0;
 }
 int available(){return 0;}
 int connected(){return up;}
 void stop(){up=false;}
};
}
'''


class Fetch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        (cls.path / "SecureClient.h").write_text('#include <cassert>\n' + SECURE)
        (cls.path / "Logging.h").write_text('#pragma once\n#define LOG_ERR(...)\n#define LOG_INF(...)\n')
        (cls.path / "Memory.h").write_text('''#pragma once
#include <memory>
#include <new>
template<class T> std::unique_ptr<T> makeUniqueNoThrow(){return std::unique_ptr<T>(new(std::nothrow) T);}
template<class T> std::unique_ptr<T> makeUniqueNoThrow(size_t n){return std::unique_ptr<T>(new(std::nothrow) std::remove_extent_t<T>[n]);}
''')
        (cls.path / "esp_err.h").write_text('#pragma once\n#define ESP_OK 0\n')
        (cls.path / "esp_task_wdt.h").write_text('''#pragma once
namespace fixture {extern int watchdog;}
inline int esp_task_wdt_status(void*){return 0;}
inline void esp_task_wdt_reset(){++fixture::watchdog;}
''')
        cls.binary = cls.path / "fetch"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I" + str(cls.path),
                        "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/network"),
                        "-I" + str(ROOT / "lib/FirmwareUpdate"),
                        str(ROOT / "src/network/OtaFetch.cpp"), str(ROOT / "lib/FirmwareUpdate/OtaHttp.cpp"),
                        str(ROOT / "test/ota_fetch/OtaFetchHarness.cpp"), "-o", str(cls.binary)], check=True,
                       capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_production_fetch_cases(self):
        cases = os.environ.get("OTA_FETCH_CASES", (
            "report,recover-no-progress,transient-head-body,bounded-idle,disconnect-resume,"
            "header-retry,resume-header-retry,wrong-range,budget,sink,manifest-single,host-policy,"
            "header-idle,repeated-disconnect,progress-reset,connect-retry,write-retry,"
            "malformed,http-error,ignored-range,wrong-length,chunked,coalesced"
        )).split(',')
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(case + ': PASS ' + result.stdout.strip())

    def test_pinned_secure_client_distinguishes_would_block_from_fatal(self):
        source = (ROOT / "freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp").read_text()
        want = source[source.index("bool isWantIo("):source.index("}  // namespace")]
        read = source[source.index("int SecureClient::read(uint8_t*"):source.index("int SecureClient::available()")]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
constexpr int WOLFSSL_ERROR_WANT_READ=2, WOLFSSL_ERROR_WANT_WRITE=3, WOLFSSL_ERROR_ZERO_RETURN=6;
struct WOLFSSL {};
int rawResult=0, tlsError=0, calls=0;
int wolfSSL_read(WOLFSSL*,void*,size_t){++calls;return rawResult;}
int wolfSSL_get_error(WOLFSSL*,int){return tlsError;}
struct SecureClient {
  bool _connected=true; WOLFSSL* _ssl=nullptr;
  int read(uint8_t*,size_t);
};
'''
        checks = r'''
int main() {
  uint8_t data[8];
  for (int want : {WOLFSSL_ERROR_WANT_READ,WOLFSSL_ERROR_WANT_WRITE}) {
    SecureClient c;rawResult=-1;tlsError=want;
    for(int i=0;i<10;++i)assert(c.read(data,8)==0 && c._connected);
    rawResult=8;assert(c.read(data,8)==8 && c._connected);
  }
  for (int fatal : {-2,-125,-308}) {
    SecureClient c;rawResult=-1;tlsError=fatal;
    assert(c.read(data,8)==-1 && !c._connected);
    int before=calls;assert(c.read(data,8)==-1 && calls==before);
  }
  SecureClient closed;rawResult=0;tlsError=WOLFSSL_ERROR_ZERO_RETURN;
  assert(closed.read(data,8)==0 && !closed._connected);
}
'''
        cpp = self.path / "secure-read.cpp"
        cpp.write_text("#include <initializer_list>\n" + harness + want + read + checks)
        binary = self.path / "secure-read"
        subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(binary)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(binary)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
