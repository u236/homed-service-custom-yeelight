FROM    127.0.0.1:5000/homed-service:latest
ARG     TARGETARCH
RUN     mkdir /etc/homed
RUN     ln -s /data/homed-custom-yeelight.conf /etc/homed/homed-custom-yeelight.conf
RUN     ln -s /data/homed.log /var/log/homed.log
COPY    homed-custom-yeelight-$TARGETARCH /usr/bin/homed-custom-yeelight
CMD     ["/usr/bin/homed-custom-yeelight"]
