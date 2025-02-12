# aqua

aqua is a cross-platform real-time audio sharing software.

> This project captures audio streams from various systems, transmits them over the network to another system as audio
> input, and plays them.
>
> The goal is to enable audio stream sharing across the vast majority of systems.

This is the author aquawius's first moderately-sized C++ project. I have high hopes for this project, hence naming it "
aqua" after the first half of my user name. I also hope `aqua` remains evergreen, like the color the word represents.

> Attention: this `readme.md` translate by DeepSeek AI, may some translation is wrong. See `readme_zh.md`.

**If you find it useful, please star this project. Issues are welcome, and I would be delighted if you submit pull
requests.**

---

#### 1. Project Introduction

The idea of creating an audio sharing tool had been brewing during my university years, but it was procrastinated until
now, finally taking shape as a farewell to my college life.

The project was initially inspired by the [audio-share](https://github.com/mkckr0/audio-share) project, which achieved
audio stream playback on Android. Their work is excellent, and I referenced parts of their code (especially the ASIO
coroutine section) during the early stages of this project. Here, I express gratitude to
the [audio-share](https://github.com/mkckr0/audio-share) project and its contributors.

#### 2. Usage

The project consists of a server and a client. You need to download and deploy them on two devices between which you
want to share audio. Both devices must support bidirectional communication.

###### Sharing End (Server):

> Start the aqua server, specifying the IP address to bind (defaults to the primary LAN IP if unspecified) and port (
> default: 10120).
>
> The server will listen the default device for audio and stream it over the network. For cross-subnet usage, bind to
`0.0.0.0`.

###### Playback End (Client):

> Start the aqua-client, specifying the server’s IP address (required) and port (default: 10120), along with the local
> IP address (defaults to the primary LAN IP if unspecified) and port (random port between 49152-65535 by default).
>
> You will then hear the audio captured by the server.
>

#### 3. Technical Architecture

Client and Server both have: gRPC services, Network services, Capture/playback services.

Client-Specific: Adaptive buffer.

Server-Specific: Session management.

![image-20250207211847020](./readme.assets/image-20250207211847020.png)

#### 4. Current Project Status

##### Completed Features:

###### aqua Server:

> - [x] Linux PipeWire capture support, (captrue PipeWire Sink stream, auto stream redirecting(PipeWire implements))
>
> - [x] Windows WASAPI capture support, stream routing feature support (Cannot change stream format).
>
> - [x] IPv4 support
>
> - [x] Session management

###### aqua Client:

> - [x] Linux PipeWire playback support
>
> - [x] Windows WASAPI playback support, stream routing feature support (Cannot change stream format).
> 
> - [x] IPv4 support
>
> - [x] Keep-alive mechanism (paired with server session management)
>
> - [x] Adaptive buffer (resists network fluctuations)

###### Unfinished/Limited Features:

> - [ ] Considering IPv6 support.
>
> - [ ] Public network NAT clients currently require manual IP/port configuration (design flaw affecting public network
    use; LAN users unaffected).
>
> - [ ] Server (aqua) and client (aqua-client) will be merged in the future.
>
> - [ ] Currently, only Linux pipewire capture and pipewire playback are supported. Windows capture is supported, but
    Windows playback has not been implemented yet (planned for the next version).
>
> - [ ] The format of the audio stream is currently immutable. On the Windows side, it needs to be specified as 48000
    bits and 2 channels in advance (otherwise, undefined behavior may occur and cause a crash).
>
> - [ ] No GUI (Qt6 planned).
>
> - [ ] Android support unlikely (no current expertise; overlaps with audio-share).
>
> - [ ] Currently, there is a strong dependency on gRPC for communication. When gRPC communication times out, it cannot
    notify the network thread in a short time. This means that if the communication is interrupted, the client will wait
    for a relatively long time before taking an exit action.


---

###### Libraries Used:

- [spdlog](https://github.com/gabime/spdlog)和[fmt](https://github.com/fmtlib/fmt)（spdlog bind）
- [cxxopts](https://github.com/jarro2783/cxxopts)
- [Boost](https://www.boost.org/)
- [gRPC](https://github.com/grpc/grpc)和[Protobuf](https://github.com/protocolbuffers/protobuf)
- [PipeWire](https://www.pipewire.org/)

Thanks to these awesome libraries and their contributors.

###### License:

The license has not yet been finalized. If dependencies impose stricter licenses, this project will comply accordingly.
