#pragma comment(lib, "ws2_32.lib")

#define WIN32_LEAN_AND_MEAN   // 排除不相干的 API，包括 winsock.h

#include <Windows.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>  // ⭐ 添加 vector

#define EXIT_ON_ERROR(hres)  \
              if (FAILED(hres)) { goto Exit; }
#define SAFE_RELEASE(punk)  \
              if ((punk) != nullptr)  \
                { (punk)->Release(); (punk) = nullptr; }

// ========== RTP 配置 ==========
// ⭐ 修改：使用 239.255.0.1（管理员范围多播地址，兼容性更好）
// 224.0.0.1 是"所有主机组"保留地址，发送可能受系统/驱动限制
const char* MULTICAST_ADDR = "224.0.0.1";
const int PORT = 1900;
const int PAYLOAD_TYPE = 10;
const UINT32 SSRC = 0x12345678;
const int MTU = 1000; // 与Android端保持一致
// =============================

// 全局变量
SOCKET g_rtpSocket = INVALID_SOCKET;
UINT16 g_rtpSeq = 0;
UINT32 g_rtpTimestamp = 0;
UINT32 g_audioFrameSize = 0;  // ⭐ 添加：每帧字节数

// 构建 RTP 头
void CreateRTPHeader(BYTE* header, UINT16 seq, UINT32 timestamp) {
    header[0] = 0x80;  // Version=2
    header[1] = PAYLOAD_TYPE;
    *(UINT16*)(header + 2) = htons(seq);
    *(UINT32*)(header + 4) = htonl(timestamp);
    *(UINT32*)(header + 8) = htonl(SSRC);
}

// SendRTP 函数中的时间戳修复
bool SendRTP(BYTE* pcmData, UINT32 pcmBytes) {
    if (g_rtpSocket == INVALID_SOCKET || pcmData == nullptr) return false;

    // 确保数据包大小不超过MTU
    const int MAX_PCM_BYTES = MTU - 12; // 12字节RTP头部
    UINT32 remainingBytes = pcmBytes;
    UINT32 offset = 0;

    while (remainingBytes > 0) {
        UINT32 currentBytes = remainingBytes > MAX_PCM_BYTES ? MAX_PCM_BYTES : remainingBytes;

        // 确保currentBytes是g_audioFrameSize的整数倍
        currentBytes = (currentBytes / g_audioFrameSize) * g_audioFrameSize;
        if (currentBytes == 0) break;

        std::vector<BYTE> packet(12 + currentBytes);
        CreateRTPHeader(packet.data(), g_rtpSeq, g_rtpTimestamp);
        memcpy(packet.data() + 12, pcmData + offset, currentBytes);

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, MULTICAST_ADDR, &addr.sin_addr);

        int result = sendto(g_rtpSocket, (char*)packet.data(), 12 + currentBytes, 0,
            (sockaddr*)&addr, sizeof(addr));

        if (result == SOCKET_ERROR) {
            int err = WSAGetLastError();
            std::cerr << "❌ sendto 失败: " << err;
            switch (err) {
            case WSAENETDOWN: std::cerr << " (网络不可用)"; break;
            case WSAEACCES:   std::cerr << " (权限/防火墙阻止)"; break;
            case WSAEFAULT:   std::cerr << " (地址参数错误)"; break;
            case WSAEMSGSIZE: std::cerr << " (包超过MTU)"; break;
            case WSAENETUNREACH: std::cerr << " (网络不可达)"; break;
            default: break;
            }
            std::cerr << "\n";
            return false;
        }

        // ⭐ 修复：时间戳增量 = 帧数（每个立体声帧 = 2 样本，但时间戳按样本计）
        UINT32 frames = currentBytes / g_audioFrameSize;
        g_rtpSeq = (g_rtpSeq + 1) & 0xFFFF;
        g_rtpTimestamp += frames;  // ⭐ 关键修复

        remainingBytes -= currentBytes;
        offset += currentBytes;
    }

    return true;
}

// 小端转大端（16-bit PCM）
void ConvertToBigEndian(BYTE* data, UINT32 byteCount) {
    for (UINT32 i = 0; i < byteCount; i += 2) {
        BYTE temp = data[i];
        data[i] = data[i + 1];
        data[i + 1] = temp;
    }
}

int main()
{
    // ⭐ 修复：所有变量在开头声明
    HRESULT hr;
    UINT32 counter = 0;              // ⭐ 关键修复
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    WAVEFORMATEX* pWaveFormat = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    UINT32 audioFrameSize = 0;
    WAVEFORMATEX* pDesiredFormat = nullptr;
    WAVEFORMATEX* pSupportedFormat = nullptr;

    // 初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "❌ WSAStartup 失败\n";
        return 1;
    }

    g_rtpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_rtpSocket == INVALID_SOCKET) {
        std::cerr << "❌ socket 创建失败: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // ⭐ 新增：多播 Socket 配置（解决 224.0.0.1 发送受限问题）

    // 1. 设置 TTL（Time To Live）：2 表示可跨子网，1 表示仅限本地链路
    int ttl = 2;
    if (setsockopt(g_rtpSocket, IPPROTO_IP, IP_MULTICAST_TTL,
        (char*)&ttl, sizeof(ttl)) == SOCKET_ERROR) {
        std::cerr << "⚠️ 设置 IP_MULTICAST_TTL 失败: " << WSAGetLastError() << "\n";
    }

    // 2. 允许接收自己发送的多播包（调试时开启，生产环境可设为 FALSE）
    BOOL loopback = TRUE;
    if (setsockopt(g_rtpSocket, IPPROTO_IP, IP_MULTICAST_LOOP,
        (char*)&loopback, sizeof(loopback)) == SOCKET_ERROR) {
        std::cerr << "⚠️ 设置 IP_MULTICAST_LOOP 失败: " << WSAGetLastError() << "\n";
    }

    // 3. 指定多播发送接口（多网卡环境必需，INADDR_ANY 让系统自动选择默认接口）
    struct in_addr localInterface;
    localInterface.s_addr = htonl(INADDR_ANY);  // 使用默认网络接口
    // 如需指定具体网卡，取消下面注释并修改IP：
    // localInterface.s_addr = inet_addr("192.168.1.100");
    if (setsockopt(g_rtpSocket, IPPROTO_IP, IP_MULTICAST_IF,
        (char*)&localInterface, sizeof(localInterface)) == SOCKET_ERROR) {
        std::cerr << "⚠️ 设置 IP_MULTICAST_IF 失败: " << WSAGetLastError() << "\n";
    }

    // 4. ⭐ 可选但推荐：bind 到本地地址（某些网卡驱动要求先 bind 才能发送多播）
    sockaddr_in localAddr = {};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);  // 绑定所有本地接口
    localAddr.sin_port = htons(0);  // 0 表示让系统自动分配端口
    if (bind(g_rtpSocket, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
        std::cerr << "⚠️ bind 本地地址失败: " << WSAGetLastError() << "（可忽略，继续尝试发送）\n";
    }

    std::cout << "✅ RTP Socket 已初始化（多播配置完成）\n";
    std::cout << "📡 目标: " << MULTICAST_ADDR << ":" << PORT << "\n\n";

    // 初始化 COM
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EXIT_ON_ERROR(hr)

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    EXIT_ON_ERROR(hr)

        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr)

        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr)

        // 获取系统默认格式
        hr = pAudioClient->GetMixFormat(&pWaveFormat);
    EXIT_ON_ERROR(hr)

        // 强制设置为 48000Hz, 16bit, 2 通道格式
        // 创建一个新的WAVEFORMATEX结构，而不是基于原始格式
        pDesiredFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (pDesiredFormat) {
        memset(pDesiredFormat, 0, sizeof(WAVEFORMATEX));
        pDesiredFormat->wFormatTag = WAVE_FORMAT_PCM;
        pDesiredFormat->nChannels = 2;
        pDesiredFormat->nSamplesPerSec = 48000;
        pDesiredFormat->wBitsPerSample = 16;
        pDesiredFormat->nBlockAlign = pDesiredFormat->nChannels * (pDesiredFormat->wBitsPerSample / 8);
        pDesiredFormat->nAvgBytesPerSec = pDesiredFormat->nSamplesPerSec * pDesiredFormat->nBlockAlign;

        // 检查格式是否支持
        hr = pAudioClient->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            pDesiredFormat,
            &pSupportedFormat
        );

        if (hr == S_OK) {
            // 格式支持，使用我们设置的格式
            CoTaskMemFree(pWaveFormat);
            pWaveFormat = pDesiredFormat;
            std::cout << "✅ 使用自定义格式: 2ch, 48000Hz, 16bit\n\n";
        }
        else {
            // 格式不支持，使用系统默认格式
            CoTaskMemFree(pDesiredFormat);
            std::cout << "⚠️  自定义格式不支持，使用系统默认格式\n";
            std::cout << "🎵 音频格式: " << pWaveFormat->nChannels << "ch, "
                << pWaveFormat->nSamplesPerSec << "Hz, "
                << pWaveFormat->wBitsPerSample << "bit\n\n";
        }
    }
    else {
        // 内存分配失败，使用系统默认格式
        std::cout << "⚠️  内存分配失败，使用系统默认格式\n";
        std::cout << "🎵 音频格式: " << pWaveFormat->nChannels << "ch, "
            << pWaveFormat->nSamplesPerSec << "Hz, "
            << pWaveFormat->wBitsPerSample << "bit\n\n";
    }

    // 释放系统分配的支持格式
    if (pSupportedFormat) {
        CoTaskMemFree(pSupportedFormat);
    }

    // 注意：我们总是发送16bit PCM，所以固定frame size为4字节（2通道 * 2字节）
    g_audioFrameSize = 2 * 2; // 2通道 * 2字节/通道

    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000, 0, pWaveFormat, nullptr);
    EXIT_ON_ERROR(hr)

        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    EXIT_ON_ERROR(hr)

        hr = pAudioClient->Start();
    EXIT_ON_ERROR(hr)

        std::cout << "🎙️ WASAPI Loopback 已启动\n";
    std::cout << "📤 发送 RTP 到 " << MULTICAST_ADDR << ":" << PORT << "\n\n";

    // 主循环
    while (true) {
        BYTE* pData = nullptr;
        UINT32 numFramesAvailable = 0;
        DWORD flags = 0;

        hr = pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr) || numFramesAvailable == 0) {
            Sleep(10);
            continue;
        }

        hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
        if (FAILED(hr) || pData == nullptr) break;

        if (numFramesAvailable > 0) {
            UINT32 totalBytes = numFramesAvailable * g_audioFrameSize;
            std::vector<BYTE> pcmBuffer;
            UINT32 outputBytes;

            // 转换为16bit PCM
            if (pWaveFormat->wBitsPerSample == 32) {
                // 32bit转16bit
                outputBytes = numFramesAvailable * 2 * 2; // 2通道 * 2字节
                pcmBuffer.resize(outputBytes);
                float* input = reinterpret_cast<float*>(pData);
                int16_t* output = reinterpret_cast<int16_t*>(pcmBuffer.data());

                for (UINT32 i = 0; i < numFramesAvailable * 2; i++) {
                    // 32bit float转16bit int
                    float sample = input[i];
                    if (sample > 1.0f) sample = 1.0f;
                    if (sample < -1.0f) sample = -1.0f;
                    output[i] = static_cast<int16_t>(sample * 32767.0f);
                }

                // 转换为大端
                ConvertToBigEndian(pcmBuffer.data(), outputBytes);
            }
            else if (pWaveFormat->wBitsPerSample == 16) {
                // 直接使用16bit
                outputBytes = totalBytes;
                pcmBuffer.resize(outputBytes);
                memcpy(pcmBuffer.data(), pData, outputBytes);

                // 转换为大端
                ConvertToBigEndian(pcmBuffer.data(), outputBytes);
            }
            else {
                // 其他格式，使用默认处理
                outputBytes = totalBytes;
                pcmBuffer.resize(outputBytes);
                memcpy(pcmBuffer.data(), pData, outputBytes);
            }

            SendRTP(pcmBuffer.data(), outputBytes);

            counter++;  // ⭐ 现在可以安全使用
            if (counter % 100 == 0) {
                std::cout << "📤 已发送 " << counter << " 包\n";
            }
        }

        hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
        if (FAILED(hr)) break;
    }

    pAudioClient->Stop();

Exit:
    std::cout << "\n🔚 清理资源...\n";

    if (g_rtpSocket != INVALID_SOCKET) {
        closesocket(g_rtpSocket);
        WSACleanup();
    }

    // 释放分配的格式资源
    if (pDesiredFormat) {
        CoTaskMemFree(pDesiredFormat);
    }
    if (pSupportedFormat) {
        CoTaskMemFree(pSupportedFormat);
    }

    CoTaskMemFree(pWaveFormat);
    SAFE_RELEASE(pCaptureClient)
        SAFE_RELEASE(pAudioClient)
        SAFE_RELEASE(pDevice)
        SAFE_RELEASE(pEnumerator)
        CoUninitialize();

    std::cout << "✅ 已退出\n";
    return 0;
}
