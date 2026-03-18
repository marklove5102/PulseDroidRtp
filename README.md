#代码修改 使用的 字节CN auto 模式
#只支持 win 必须使用 AudioPeeperConsole 
````
启用 宏
WIN32_LEAN_AND_MEAN
添加 附加依赖项
ole32.lib;strmiids.lib;ws2_32.lib;dmoguids.lib;oleaut32.lib;
````
#声音获取 使用的下面的方法。
#https://github.com/luqiming666/AudioPeeperConsole/blob/main/AudioPeeperConsole.cpp

#android-studio-panda2-windows.exe 1.8

#ASIO
SHA-1: bd500f0a018db9a845ebaaed5c0318343ae9f497
* Add support for using Cygwin without __USE_W32_SOCKETS.


#oboe
SHA-1: d25993c5dbe4750cd8d51a3729b6def6f9d20d77
* Copy in prefab build scripts and bump version to 1.4.3
会有断言  为引用 声明问题 自己手工修复吧！

````
./ffmpeg -f dshow -i audio="virtual-audio-capturer" -codec:a pcm_s16be -ac 2 -f rtp -mtu 1000 -pkt_size 1000 rtp://192.168.2.3:1900
-mtu 1000 -pkt_size 1000 这两个参数 同时在 才不会出现 MTU不达标声音 抖的感觉
./ffmpeg -f dshow -i audio="virtual-audio-capturer" -codec:a pcm_s16be -ac 2 -f rtp -localaddr 192.168.2.4 -mtu 1000 -pkt_size 1000 rtp://224.0.0.1:1900
只有加了 -localaddr 192.168.2.4  你才可能使用 rtp://224.0.0.1:1900
````
