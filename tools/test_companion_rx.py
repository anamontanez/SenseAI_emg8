"""Production relay buffer: fragmented input, overflow, wrap and recovery."""
from pathlib import Path
import re, shutil, subprocess, tempfile, unittest
class CompanionReceive(unittest.TestCase):
    def test_buffer(self):
        source=(Path(__file__).resolve().parents[1]/"src/companion_rx_buffer.hpp").read_text()
        source=re.sub(r"#(?:include[^\n]*|pragma once)", "", source)
        source=re.sub(r"    (Result|void|uint32_t|bool|char) (\w+)\(", r"    constexpr \1 \2(", source)
        prelude="""
using uint8_t=unsigned char; using uint32_t=unsigned;
namespace std {
enum memory_order { memory_order_relaxed, memory_order_acquire, memory_order_release };
template<class T> struct atomic {
 T value; constexpr atomic(T v):value(v) {}
 constexpr T load(memory_order) const { return value; }
 constexpr void store(T v,memory_order) { value=v; }
};
}
"""
        checks=r"""
using B=CompanionRxBuffer; using R=B::Result;
constexpr R line(B& b,const char* s) {
 R r=R::None; for(unsigned i=0;s[i];++i) { R v=b.feed(s[i]); if(v!=R::None) r=v; } return r;
}
constexpr bool equal(const B::View& v,const char* s) {
 unsigned n=v.firstSize+v.secondSize;
 for(unsigned i=0;i<n;++i)
  if((i<v.firstSize?v.first[i]:v.second[i-v.firstSize])!=s[i]) return false;
 return s[n]==0;
}
#define CHECK(c) do { if(!(c)) return __LINE__; } while(0)
constexpr int check() {
 B b; B::View v{};
 CHECK(line(b,"imp:[1000,12;")==R::None);
 CHECK(!b.peek(b.snapshot(),v));
 CHECK(line(b,"2000,23]\n")==R::Accepted);
 auto limit=b.snapshot();
 CHECK(line(b,"p1:1,p2:2,temp:30\r\n")==R::Accepted);
 CHECK(b.peek(limit,v) && equal(v,"imp:[1000,12;2000,23]")); b.consume(v);
 CHECK(!b.peek(limit,v));
 CHECK(b.peek(b.snapshot(),v) && equal(v,"p1:1,p2:2,temp:30")); b.consume(v);
 for(int i=0;i<300;++i) {
  CHECK(line(b,"p1:1,p2:2,temp:3\n")==R::Accepted);
  CHECK(b.peek(b.snapshot(),v) && equal(v,"p1:1,p2:2,temp:3")); b.consume(v);
 }
 line(b,"imp:[");
 for(unsigned i=5;i<B::maxLine-1;++i) CHECK(b.feed('1')==R::None);
 CHECK(line(b,"]\n")==R::Accepted);
 CHECK(b.peek(b.snapshot(),v) && v.firstSize+v.secondSize==B::maxLine);
 int full=0;
 for(int i=0;i<150;++i) if(line(b,"p1:1,p2:2,temp:3\n")==R::Full) ++full;
 CHECK(full>0 && v.first[0]=='i' && v.first[1]=='m');
 b.consume(v); while(b.peek(b.snapshot(),v)) b.consume(v);
 CHECK(line(b,"#STATUS:bad\n")==R::Rejected);
 CHECK(line(b,"imp:[unfinished\n")==R::Rejected);
 CHECK(line(b,"p1:1,p2:2\n")==R::Rejected);
 line(b,"imp:[");
 for(unsigned i=0;i<B::maxLine+100;++i) b.feed('1');
 CHECK(line(b,"]\n")==R::None);
 line(b,"imp:[12,34"); b.losePartial();
 CHECK(line(b,"]\n")==R::None);
 CHECK(line(b,"imp:[12,34]\n")==R::Accepted);
 CHECK(b.peek(b.snapshot(),v) && equal(v,"imp:[12,34]")); b.consume(v);
 CHECK(!b.peek(b.snapshot(),v)); return 0;
}
constexpr int result=check();
static_assert(result==0,"RX failure: result identifies CHECK line");
"""
        compiler=shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/"rx.cpp"
            cpp.write_text(prelude+source+checks)
            subprocess.run([compiler,"-std=c++17","-fconstexpr-steps=3000000","-fsyntax-only",str(cpp)],check=True)
