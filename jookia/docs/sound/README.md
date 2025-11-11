Copyright 2025 John Watts <contact@jookia.org>.
Licensed under the CC0.

Quickstart
----------

If you're unfamiliar with how ALSA works in Linux, here's a quick
guide overview in point form:

- Userspace interacts sound cards
- Sound cards expose PCM interfaces and controls

This works very well for all-in-one devices, such as PCIe or USB sound
cards. But it doesn't work very well for embedded systems where we have
to join together multiple chips to get what resembles a 'card'.

For example, we may have this setup:

- I2S core on a SoC for handling PCM input and output
- Codec chip that handles digital to audio conversion
- I2C buses that talk to the codec
- GPIOs that handle resets for the codec
- Regulators that power the codec

Ideally we could make some sound card out of this that userspace can
use without worrying about anything.

The ALSA SoC layer allows us to specify each of the above components as
individual components created by the device tree (or ACPI tree) like any
other embedded component.

For example, let's say we have a board with one simple codec that talks
over I2S to the SoC. We would put the components in a device tree like
this:

    / {
        wm8782: stereo-adc {
            compatible = "wlf,wm8782";
            Vdda-supply = <&reg_vcc>;
            Vdd-supply = <&reg_vcc>;
            #sound-dai-cells = <0>;
        };
    };

    &i2s2 { /* Override of the SoC I2S node */
        pinctrl-0 = <&i2s2_pins>, <&i2s2_din_pins>;
        pinctrl-names = "default";
        status = "okay";
    };

Each of these ALSA SoC components can have:

- ALSA controls
- Supported I2S modes
- Supported audio formats
- Supported audio rates
- Dynamic audio power management rules
- Code to consume or generate a MCLK rate

At this point I suggest skimming the kernel documentation here:

https://www.kernel.org/doc/html/latest/sound/index.html

It may also be useful to skim the source code for the two devices I used
above, take a look at their snd_soc_dai_driver structures:

    sound/soc/codecs/wm8782.c
    sound/soc/sunxi/sun4i-i2s.c

You should now have a decent idea of what a component looks like and
what it can do. Something you'll be wondering is how to connect them
together. This can be done by using a sound card.

Simple sound cards
------------------

The easiest way to do this is using the simple-audio-card device.
Put something like this in your device tree:

    / {
        sound {
            compatible = "simple-audio-card";
            simple-audio-card,name = "MyCard";
            simple-audio-card,format = "i2s";
            simple-audio-card,bitclock-master = <&sound_cpu>;
            simple-audio-card,frame-master = <&sound_cpu>;
            simple-audio-card,mclk-fs = <256>;

            sound_cpu: simple-audio-card,cpu {
                sound-dai = <&i2s2>;
            };

            sound_wm8782: simple-audio-card,codec {
                sound-dai = <&wm8782>;
            };
        };
    };

This creates a sound card named "MyCard" and does the following:

- Sets the format to I2S
- Makes the CPU the MCLK and BCLK source
- Sets the MCLK speed
- Sets the CPU DAI to the i2s2 device
- Sets the codec DAI to the i2s2 device

You can include multiple pairs of CPU and codec links if you need that.

I highly recommend reading the following files for some more information
and examples of the simple-audio-card device:

    Documentation/devicetree/bindings/sound/simple-card.yaml

Over time the device tree has moved towards using a graph to represent
connections between devices directly. You can do this with
audio-graph-card2 by linking the CPU to the codec in the device tree.
This has the same result.

    / {
        sound {
            compatible = "audio-graph-card2";
            label = "MyCard";
            links = <&i2s2_port>;
        };

        wm8782: stereo-adc {
            compatible = "wlf,wm8782";
            Vdda-supply = <&reg_vcc>;
            Vdd-supply = <&reg_vcc>;
            #sound-dai-cells = <0>;

            ports {
                port {
                    wm8782_endpoint: endpoint {
                        remote-endpoint = <&i2s2_endpoint>;
                    };
                };
            };
        };
    };

    &i2s2 { /* Override of the SoC I2S node */
        pinctrl-0 = <&i2s2_pins>, <&i2s2_din_pins>;
        pinctrl-names = "default";
        status = "okay";

        ports {
            i2s2_port: port {
                i2s2_endpoint: endpoint {
                    bitclock-master;
                    frame-master;
                    frame-inversion;
                    mclk-fs = <256>;
                    remote-endpoint = <&wm8782_endpoint>;
                };
            };
        };
    };

Notice how the endpoint on the CPU (wm8782_endpoint) now includes the
format and clocking details. This format is a bit more burdensome but
turns out to be a lot more flexible.

I recommend reading the following files for more information about
audio-graph-card and audio-graph-card2:

    arch/arm64/boot/dts/renesas/ulcb-audio-graph-card.dtsi
    arch/arm64/boot/dts/renesas/ulcb-kf-audio-graph-card.dtsi
    arch/arm64/boot/dts/renesas/ulcb-audio-graph-card2-mix+split.dtsi
    arch/arm64/boot/dts/renesas/ulcb-kf-audio-graph-card2-mix+split.dtsi
    Documentation/devicetree/bindings/sound/audio-graph-card2.yaml
    Documentation/devicetree/bindings/sound/audio-graph-card.yaml
    Documentation/devicetree/bindings/sound/audio-graph-port.yaml
    Documentation/devicetree/bindings/sound/audio-graph.yaml
    Documentation/devicetree/bindings/sound/dai-params.yaml
    Documentation/devicetree/bindings/sound/tdm-slot.txt
    sound/soc/generic/audio-graph-card2.c

The documentation is a little lacking but I would highly recommend using
audio-graph-card2 for new device trees.

DAI links
---------

After looking at the device trees in the previous section you should be
able to understand the concept of linking the CPU to a codec.

This is implemented by having the sound card look over the connections
in the device tree and create DAI links between components. DAI links
are not very well documented but are important to understand before
moving on.

A DAI link contains a list of components, such as codecs or CPU
components. There must be at least one codec, it's okay to have no
CPU components in the link.

We can use the kernel's debugging output to learn more about the links
our sound card sets up. In the example above we get output like this:

    asoc-audio-graph-card2 sound: link 1, dais 2, ccnf 0
    asoc-audio-graph-card2 sound: dai_link 2034000.i2s-wm8782
    asoc-audio-graph-card2 sound:   [0] cpu0 <-> codec0
    asoc-audio-graph-card2 sound: ASoC: binding 2034000.i2s-wm8782
    sun4i-i2s 2034000.i2s: ASoC: adding Playback widget
    sun4i-i2s 2034000.i2s: ASoC: adding Capture widget
    wm8782 stereo-adc: ASoC: adding Capture widget
    asoc-audio-graph-card2 sound: ASoC: registered pcm #0 2034000.i2s-wm8782 wm8782-0
    asoc-audio-graph-card2 sound: wm8782 <-> 2034000.i2s mapping ok
    asoc-audio-graph-card2 sound: connected DAI link stereo-adc:Capture -> 2034000.i2s:Capture

But what if we had multiple I2S devices and codecs? We might have two
links to two different codecs:

    sun4i-i2s1 <-> wm8782-1
    sun4i-i2s2 <-> wm8782-2

The sound card would encapsulate both of these links and provide
userspace a way to specify which link to use for recording.

I don't have the hardware to test this, but for the purpose of
investigating DAI links we can use test-cpu and test-codec devices.
Here's an example of the above situation using test components:

    / {
        sound {
            compatible = "audio-graph-card2";
            label = "MyCard";
            links = <&test_cpu1_port &test_cpu2_port>;
        };

        test_cpu1 {
            compatible = "test-cpu";
            test_cpu1_port: port {
                test_cpu1_endpoint: endpoint {
                    remote-endpoint = <&test_codec1_endpoint>;
                };
            };
        };

        test_cpu2 {
            compatible = "test-cpu";
            test_cpu2_port: port {
                test_cpu2_endpoint: endpoint {
                    remote-endpoint = <&test_codec2_endpoint>;
                };
            };
        };

        test_codec1 {
            compatible = "test-codec";
            sound-name-prefix = "Codec1";
            port {
                test_codec1_endpoint: endpoint {
                    remote-endpoint = <&test_cpu1_endpoint>;
                };
            };
        };

        test_codec2 {
            compatible = "test-codec";
            sound-name-prefix = "Codec2";
            port {
                test_codec2_endpoint: endpoint {
                    remote-endpoint = <&test_cpu2_endpoint>;
                };
            };
        };
    };

As a side note specifying a sound-name-prefix is very helpful when
dealing with many codecs as you won't have duplicate controls for things
like volume control and input muting, instead they will be prefixed.

Anyway this gives this debug output:

    asoc-audio-graph-card2 sound: link 2, dais 4, ccnf 0
    asoc-audio-graph-card2 sound: dai_link test_cpu1.0-test_codec1.0
    asoc-audio-graph-card2 sound:   [0] cpu0 <-> codec0
    asoc-audio-graph-card2 sound: ASoC: binding test_cpu1.0-test_codec1.0
    asoc-audio-graph-card2 sound: dai_link test_cpu2.0-test_codec2.0
    asoc-audio-graph-card2 sound:   [0] cpu0 <-> codec0
    asoc-audio-graph-card2 sound: ASoC: binding test_cpu2.0-test_codec2.0
    asoc-audio-graph-card2 sound: ASoC: registered pcm #0 test_cpu1.0-test_codec1.0 test_codec1.0-0
    asoc-audio-graph-card2 sound: test_codec1.0 <-> test_cpu1 mapping ok
    asoc-audio-graph-card2 sound: ASoC: registered pcm #1 test_cpu2.0-test_codec2.0 test_codec2.0-1
    asoc-audio-graph-card2 sound: test_codec2.0 <-> test_cpu2 mapping ok
    asoc-audio-graph-card2 sound: connected DAI link test_cpu1:DAI0 Playback -> test_codec1:DAI0 Playback
    asoc-audio-graph-card2 sound: connected DAI link test_codec1:DAI0 Capture -> test_cpu1:DAI0 Capture
    asoc-audio-graph-card2 sound: connected DAI link test_cpu2:DAI0 Playback -> test_codec2:DAI0 Playback
    asoc-audio-graph-card2 sound: connected DAI link test_codec2:DAI0 Capture -> test_cpu2:DAI0 Capture

This creates 2 DAI links, one for each CPU component. This is expected
as we have two ports specified in the 'links' property.

So far all the DAI links contain one CPU and one codec component. This
is all simple-audio-card and audio-graph-card are designed to do outside
of some DPCM support. For more complex links we will have to use
audio-graph-card2 so it's worth learning.

I would suggest reading this email by the device author Kuninori
Morimoto. It shows the differences between the each devices:

https://lore.kernel.org/all/87fsw124wn.wl-kuninori.morimoto.gx@renesas.com/

He has also given a talk explaining the history of these cards and ALSA
SoC infrastructure:

https://www.youtube.com/watch?v=usRwGpJJkpk

Now that we understand what DAI links are I will talk about creating
some real-world links in the next sections.

TDM multiplexing
----------------

I2S generally supports sending only two channels of audio: Left and
right channels, usually 32-bit. But some codecs can sample many more and
output them as a long set of bits each frame. This is known as TDM
multiplexing and would look something like this in the device tree:

    / {
        sound {
            compatible = "audio-graph-card2";
            label = "MyCard";
            links = <&i2s2_port>;
        };
    };

    &i2c2 {
        /* Device data omitted */
        cs5368: tdm-adc@0x4c {
            /* Device data omitted */
            ports {
                port {
                    cs5368_endpoint: endpoint {
                        dai-tdm-slot-num = <8>;
                        dai-tdm-slot-width = <32>;
                        remote-endpoint = <&i2s2_endpoint>;
                    };
                };
            };
    };

    &i2s2 {
        /* Device data omitted */
        ports {
            i2s2_port: port {
                i2s2_endpoint: endpoint {
                    frame-master;
                    frame-inversion;
                    mclk-fs = <256>;
                    dai-format = "i2s";
                    dai-tdm-slot-num = <8>;
                    dai-tdm-slot-width = <32>;
                    remote-endpoint = <&cs5368_endpoint>;
                };
            };
        };
    };

This can be accomplished with simple-audio-card too.

This gives the following debug output:

    asoc-audio-graph-card2 sound: link 1, dais 2, ccnf 0
    asoc-audio-graph-card2 sound: dai_link 2034000.i2s-cs5368
    asoc-audio-graph-card2 sound:   [0] cpu0 <-> codec0
    asoc-audio-graph-card2 sound: ASoC: binding 2034000.i2s-cs5368
    asoc-audio-graph-card2 sound: ASoC: registered pcm #0 2034000.i2s-cs5368 cs5368-0
    asoc-audio-graph-card2 sound: cs5368 <-> 2034000.i2s mapping ok
    asoc-audio-graph-card2 sound: connected DAI link cs5368.0-004c:Capture -> 2034000.i2s:Capture

But just because the DAIs link together doesn't mean we can actually use
them. At runtime we request audio with a set of properties such as channels,
sample rate, and bit depth. The kernel will then check if each DAI
supports these properties before allowing you to use them to play or
capture audio.

In this case we are able to successfully capture audio as both the I2S
device and the codec support 8 channel 32-bit audio at whatever sample
rates we pick, such as 48 kHz.

Something to note is that the properties given to the I2S and codec
endpoints are not part of the DAI link. They are used instead by
audio-graph-card2 to configure the component attached to that endpoint
or configure the card itself for things like clocking.

This means specify you must specify TDM properties on each component so
all components configure themselves to work in the chip's TDM mode.

N:M multiplexing
----------------

So far I've discussed the case of creating and using DAI links with one
CPU component and one codec component. But say we have this setup:

- Two codecs that record 2 channel audio each to their own I2S stream
- A CPU component that can mux multiple streams into a single stream

We would configure the CPU like this:

- CPU channel 0 <- I2S stream 0 <- Codec 1 channel 0
- CPU channel 1 <- I2S stream 0 <- Codec 1 channel 1
- CPU channel 2 <- I2S stream 1 <- Codec 2 channel 0
- CPU channel 3 <- I2S stream 1 <- Codec 2 channel 1

Here's how to create an N:M mapping using audio-graph-card2:

    / {
        sound {
            compatible = "audio-graph-card2";
            label = "MyCard";
            links = <&cpu_port_0>;

            multi {
                #address-cells = <1>;
                #size-cells = <0>;

                ports@0 {
                    #address-cells = <1>;
                    #size-cells = <0>;
                    reg = <0x0>;

                    cpu_port_0: port@0 {
                        reg = <0x0>;
                        cpu_endpoint_0: endpoint {
                            remote-endpoint = <&codec_endpoint_0>;
                        };
                    };

                    port@1 {
                        reg = <0x1>;
                        cpu_endpoint_1: endpoint {
                            remote-endpoint = <&test_cpu_endpoint>;
                        };
                    };
                };

                ports@1 {
                    #address-cells = <1>;
                    #size-cells = <0>;
                    reg = <0x1>;

                    port@0 {
                        reg = <0x0>;
                        codec_endpoint_0: endpoint {
                            remote-endpoint = <&cpu_endpoint_0>;
                        };
                    };

                    port@1 {
                        reg = <0x1>;
                        codec_endpoint_1: endpoint {
                            remote-endpoint = <&test_codec1_endpoint>;
                        };
                    };

                    port@2 {
                        reg = <0x2>;
                        codec_endpoint_2: endpoint {
                            remote-endpoint = <&test_codec2_endpoint>;
                        };
                    };
                };
            };
        };

        test_cpu {
            compatible = "test-cpu";
            test_cpu_port: port {
                test_cpu_endpoint: endpoint {
                    remote-endpoint = <&cpu_endpoint_1>;
                };
            };
        };

        test_codec1 {
            compatible = "test-codec";
            sound-name-prefix = "Codec1";
            port {
                test_codec1_endpoint: endpoint {
                    remote-endpoint = <&codec_endpoint_1>;
                };
            };
        };

        test_codec2 {
            compatible = "test-codec";
            sound-name-prefix = "Codec2";
            port {
                test_codec2_endpoint: endpoint {
                    remote-endpoint = <&codec_endpoint_2>;
                };
            };
        };
    };

This gives us the following debug output:

    asoc-audio-graph-card2 sound: link 1, dais 3, ccnf 0
    asoc-audio-graph-card2 sound: dai_link test_cpu.0-test_codec1.0_multi
    asoc-audio-graph-card2 sound:   [0] cpu0 <-> codec0
    asoc-audio-graph-card2 sound:   [1] cpu0 <-> codec1
    asoc-audio-graph-card2 sound: ASoC: binding test_cpu.0-test_codec1.0_multi
    asoc-audio-graph-card2 sound: ASoC: registered pcm #0 test_cpu.0-test_codec1.0_multi multicodec-0
    asoc-audio-graph-card2 sound: multicodec <-> test_cpu mapping ok
    asoc-audio-graph-card2 sound: connected DAI link test_cpu:DAI0 Playback -> test_codec1:DAI0 Playback
    asoc-audio-graph-card2 sound: connected DAI link test_codec1:DAI0 Capture -> test_cpu:DAI0 Capture
    asoc-audio-graph-card2 sound: connected DAI link test_cpu:DAI0 Playback -> test_codec2:DAI0 Playback
    asoc-audio-graph-card2 sound: connected DAI link test_codec2:DAI0 Capture -> test_cpu:DAI0 Capture

We've constructed a link between the test CPU and both test codecs.
This link however has a subtle gotcha: It will pass the channels you use
on the CPU component straight through to the attached codecs without
checking for compatibility.
So asking for recording 6 channels from our CPU component will ask to
record 6 channels from each of our codecs too.

This works for the test components so we won't get an error, but for
our real example this won't work. We have to do the following first:

1. Specify that everything on the link is 2 channel audio
2. We need to specify that CPU audio data is 6 channel audio

For the first problem where we specify the channels and data on the link
we already have a solution for that: Specifying TDM properties. It's a
little strange but by specifying TDM properties for just two channels
we've now clearly defined how many channels each component is using.

The second problem of specifying CPU audio channels is solved by the DAI
link allowing us to request a channel count that isn't supported by the
codecs. We can request 6 channels but the ALSA code will fix up the
codec channel requests based on the TDM information we've provided.

By making changes like these:

        test_cpu {
            compatible = "test-cpu";
            test_cpu_port: port {
                test_cpu_endpoint: endpoint {
                    mclk-fs = <32>;
                    dai-format = "i2s";
                    dai-tdm-slot-num = <2>;
                    dai-tdm-slot-width = <32>;
                    remote-endpoint = <&cpu_endpoint_1>;
                };
            };
        };

        test_codec1 {
            compatible = "test-codec";
            port {
                test_codec1_endpoint: endpoint {
                    dai-tdm-slot-num = <2>;
                    dai-tdm-slot-width = <32>;
                    remote-endpoint = <&codec_endpoint_1>;
                };
            };
        };

        test_codec2 {
            compatible = "test-codec";
            port {
                test_codec2_endpoint: endpoint {
                    dai-tdm-slot-num = <2>;
                    dai-tdm-slot-width = <32>;
                    remote-endpoint = <&codec_endpoint_2>;
                };
            };
        };

We can now read 6 channels of audio from the CPU component while only
handling 2 channels over I2S. Be aware that you will need to configure
your CPU component to mux these channels correctly.

Real world T113 example
-----------------------

Here's an example of combining both TDM and multiple CS5368 codecs to
record 16 channels of audio at once on a T113. This is done by passing
each codec's I2S TDM data to a separate DIN pin and confusing the T113's
I2S block to mix these together in to a single stream.

    / {
	sound {
		compatible = "audio-graph-card2";
		label = "ADCArray";
		links = <&cpu_port_0>;

		multi {
			#address-cells = <1>;
			#size-cells = <0>;

			ports@0 {
				#address-cells = <1>;
				#size-cells = <0>;
				reg = <0x0>;

				cpu_port_0: port@0 {
					reg = <0x0>;
					cpu_endpoint_0: endpoint {
						remote-endpoint = <&codec_endpoint_0>;
					};
				};

				port@1 {
					reg = <0x1>;
					cpu_endpoint_1: endpoint {
						remote-endpoint = <&i2s2_endpoint>;
					};
				};
			};

			ports@1 {
				#address-cells = <1>;
				#size-cells = <0>;
				reg = <0x1>;

				port@0 {
					reg = <0x0>;
					codec_endpoint_0: endpoint {
						remote-endpoint = <&cpu_endpoint_0>;
					};
				};

				port@1 {
					reg = <0x1>;
					codec_endpoint_1: endpoint {
						remote-endpoint = <&cs5368_1_endpoint>;
					};
				};

				port@2 {
					reg = <0x2>;
					codec_endpoint_2: endpoint {
						remote-endpoint = <&cs5368_2_endpoint>;
					};
				};
			};
		};
	};
    };

    &i2c2 {
            cs5368-1@4c {
                    reg = <0x4c>;
                    compatible = "cirrus,cs5368";
                    vdd-supply = <&reg_vcc5v>;
                    vdda-supply = <&reg_3v3>;
                    reset-gpios = <&pio 4 11 GPIO_ACTIVE_LOW>; /* PE11 */
                    #sound-dai-cells = <0>;
                    sound-name-prefix = "ADC1";
                    ports {
                            port {
                                    cs5368_1_endpoint: endpoint {
                                            dai-tdm-slot-num = <8>;
                                            dai-tdm-slot-width = <32>;
                                            remote-endpoint = <&codec_endpoint_1>;
                                    };
                            };
                    };
            };

            cs5368-2@4e {
                    reg = <0x4e>;
                    compatible = "cirrus,cs5368";
                    vdd-supply = <&reg_vcc5v>;
                    vdda-supply = <&reg_3v3>;
                    reset-gpios = <&pio 4 12 GPIO_ACTIVE_LOW>; /* PE12 */
                    #sound-dai-cells = <0>;
                    sound-name-prefix = "ADC2";
                    ports {
                            port {
                                    cs5368_2_endpoint: endpoint {
                                            dai-tdm-slot-num = <8>;
                                            dai-tdm-slot-width = <32>;
                                            remote-endpoint = <&codec_endpoint_2>;
                                    };
                            };
                    };
            };
    };

    &i2s2 {
            pinctrl-0 = <&i2s2_pins>, <&i2s2_din_pins>;
            pinctrl-names = "default";
            status = "okay";
            allwinner,channel-slots = <7 5 3 1 6 4 2 0 1 2 3 4 5 6 7>;
            allwinner,channel-dins = <0 0 0 0 0 0 0 0 2 2 2 2 2 2 2 2>;
            ports {
                    port {
                            i2s2_endpoint: endpoint {
                                    bitclock-master;
                                    frame-master;
                                    frame-inversion;
                                    mclk-fs = <256>;
                                    dai-format = "i2s";
                                    dai-tdm-slot-num = <8>;
                                    dai-tdm-slot-width = <32>;
                                    remote-endpoint = <&cpu_endpoint_1>;
                            };
                    };
            };
    };

I haven't tested this, but it should work. What do you think?
