// Hidden WGL context: validate the production OpenGL shader and indirect consumer.
#include <glad/glad.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
using u32=uint32_t;
void CheckGL() {
    if(const auto error=glGetError();error!=GL_NO_ERROR)
        throw std::runtime_error("OpenGL error "+std::to_string(error));
}
GLuint Program(const std::string& text) {
    GLuint shader=glCreateShader(GL_COMPUTE_SHADER);const char* source=text.c_str();
    glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);GLint ok=0;
    glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[8192]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);throw std::runtime_error(log);}
    GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);
    glDeleteShader(shader);glGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok){char log[8192]{};glGetProgramInfoLog(program,sizeof(log),nullptr,log);throw std::runtime_error(log);}
    return program;
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("production_shader.comp");
        WNDCLASSA wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcA;
        wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="NxemuIndirectTest";
        if(!RegisterClassA(&wc))throw std::runtime_error("RegisterClass");
        HWND window=CreateWindowA(wc.lpszClassName,"",WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
        HDC dc=GetDC(window);PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;
        pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=24;
        if(!SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd))throw std::runtime_error("Pixel format");
        HGLRC context=wglCreateContext(dc);
        if(!context||!wglMakeCurrent(dc,context)||!gladLoadGL())throw std::runtime_error("OpenGL context");
        std::cout<<"GPU "<<glGetString(GL_RENDERER)<<" GL "<<glGetString(GL_VERSION)<<'\n';
        std::ifstream stream(argv[1]);std::string code{std::istreambuf_iterator<char>(stream),{}};
        GLuint convert=Program(code);
        GLuint counter=Program("#version 450\nlayout(local_size_x=1) in;"
                               "layout(binding=0,std430) buffer Count {uint n;};"
                               "void main(){atomicAdd(n,1u);}");
        std::array<GLuint,4> buffers{};glCreateBuffers(4,buffers.data());
        for(auto buffer:buffers)glNamedBufferStorage(buffer,16,nullptr,GL_DYNAMIC_STORAGE_BIT);
        int tests=0,failures=0;
        auto test=[&](const char* name,u32 x,u32 yz,std::array<u32,4> p,u32 mask,std::array<u32,3> expected){
            const std::array<u32,4> zero{};std::array<u32,4> x_data{},yz_data{};
            x_data[p[0]]=x;yz_data[p[1]]=yz;
            glNamedBufferSubData(buffers[0],0,16,x_data.data());
            glNamedBufferSubData(buffers[1],0,16,yz_data.data());
            glNamedBufferSubData(buffers[3],0,16,zero.data());
            for(u32 i=0;i<3;++i)glBindBufferBase(GL_SHADER_STORAGE_BUFFER,i,buffers[i]);
            glUseProgram(convert);glUniform4uiv(0,1,p.data());glUniform1ui(1,mask);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);glDispatchCompute(1,1,1);
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            // Restore the consumer program and descriptor, as the rasterizer does.
            glUseProgram(counter);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffers[3]);
            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,buffers[2]);glDispatchComputeIndirect(0);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            std::array<u32,3> observed{};u32 count=0;
            glGetNamedBufferSubData(buffers[2],0,12,observed.data());
            glGetNamedBufferSubData(buffers[3],0,4,&count);CheckGL();
            bool ok=observed==expected&&count==expected[0]*expected[1]*expected[2];
            ++tests;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" invocations="<<count<<'\n';
        };
        test("X_only",693,0,{0,0,0,0x00010001},1,{693,1,1});
        test("YZ_only",0,0x00030002,{0,0,7,0},2,{7,2,3});
        test("zero_Z",1,1,{0,0,0,0},3,{1,1,0});
        test("zero_X",0,0x00010001,{0,0,1,1},3,{0,1,1});
        test("zero_Y",1,0x00010000,{0,0,1,1},3,{1,0,1});
        test("X_reserved_flag",0x80000005,0x00020003,{0,0,0,0},3,{5,3,2});
        test("separate_offsets",2,0x00050003,{1,2,0,0},3,{2,3,5});
        test("static_dimensions",999,999,{0,0,4,0x00020003},0,{4,3,2});
        for(u32 i=1;i<=10;++i)test("varying_dimensions",i,(i+1)|((i+2)<<16),{0,0,0,0},3,{i,i+1,i+2});
        glDeleteBuffers(4,buffers.data());glDeleteProgram(convert);glDeleteProgram(counter);
        wglMakeCurrent(nullptr,nullptr);wglDeleteContext(context);ReleaseDC(window,dc);
        DestroyWindow(window);UnregisterClassA(wc.lpszClassName,wc.hInstance);
        std::cout<<"tests="<<tests<<" failures="<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
