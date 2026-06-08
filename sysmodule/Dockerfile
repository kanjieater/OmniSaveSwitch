FROM devkitpro/devkita64:latest

ENV DEVKITPRO=/opt/devkitpro
ENV PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:/opt/devkitpro/devkitA64/bin:${PATH}

RUN dkp-pacman -S --noconfirm switch-curl switch-zlib

WORKDIR /src
