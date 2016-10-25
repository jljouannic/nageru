CXX=g++
PROTOC=protoc
INSTALL=install
EMBEDDED_BMUSB=no
PKG_MODULES := Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL libusb-1.0 movit lua52 libmicrohttpd epoxy x264 protobuf
CXXFLAGS ?= -O2 -g -Wall  # Will be overridden by environment.
CXXFLAGS += -std=gnu++11 -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\" -Idecklink/

ifeq ($(EMBEDDED_BMUSB),yes)
  CPPFLAGS += -Ibmusb/
else
  PKG_MODULES += bmusb
endif
LDLIBS=$(shell pkg-config --libs $(PKG_MODULES)) -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil -lswscale -lavresample -lzita-resampler -lasound -ldl

# Qt objects
OBJS_WITH_MOC = glwidget.o mainwindow.o vumeter.o lrameter.o compression_reduction_meter.o correlation_meter.o aboutdialog.o input_mapping_dialog.o midi_mapping_dialog.o nonlinear_fader.o
OBJS += $(OBJS_WITH_MOC)
OBJS += $(OBJS_WITH_MOC:.o=.moc.o) ellipsis_label.moc.o clickable_label.moc.o
OBJS += vu_common.o piecewise_interpolator.o main.o
OBJS += midi_mapper.o midi_mapping.pb.o

# Mixer objects
AUDIO_MIXER_OBJS = audio_mixer.o alsa_input.o alsa_pool.o ebu_r128_proc.o stereocompressor.o resampling_queue.o flags.o correlation_measurer.o filter.o input_mapping.o state.pb.o
OBJS += mixer.o pbo_frame_allocator.o context.o ref_counted_frame.o theme.o httpd.o flags.o image_input.o alsa_output.o disk_space_estimator.o $(AUDIO_MIXER_OBJS)

# Streaming and encoding objects
OBJS += quicksync_encoder.o x264_encoder.o x264_speed_control.o video_encoder.o metacube2.o mux.o audio_encoder.o ffmpeg_raii.o

# DeckLink
OBJS += decklink_capture.o decklink/DeckLinkAPIDispatch.o

# bmusb
ifeq ($(EMBEDDED_BMUSB),yes)
  OBJS += bmusb/bmusb.o bmusb/fake_capture.o
endif

# Benchmark program.
BM_OBJS = benchmark_audio_mixer.o $(AUDIO_MIXER_OBJS) flags.o

%.o: %.cpp
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
%.o: %.cc
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
%.pb.cc %.pb.h : %.proto
	$(PROTOC) --cpp_out=. $<

%.h: %.ui
	uic $< -o $@

%.moc.cpp: %.h
	moc $< -o $@

all: nageru benchmark_audio_mixer

nageru: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)
benchmark_audio_mixer: $(BM_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Extra dependencies that need to be generated.
aboutdialog.o: ui_aboutdialog.h
alsa_pool.o: state.pb.h
audio_mixer.o: state.pb.h
input_mapping.o: state.pb.h
input_mapping_dialog.o: ui_input_mapping.h
mainwindow.o: ui_mainwindow.h ui_display.h ui_audio_miniview.h ui_audio_expanded_view.h ui_midi_mapping.h
mainwindow.o: midi_mapping.pb.h
midi_mapper.o: midi_mapping.pb.h
midi_mapping_dialog.o: ui_midi_mapping.h midi_mapping.pb.h

DEPS=$(OBJS:.o=.d) $(BM_OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(BM_OBJS) $(DEPS) nageru benchmark_audio_mixer ui_aboutdialog.h ui_mainwindow.h ui_display.h ui_about.h ui_audio_miniview.h ui_audio_expanded_view.h ui_input_mapping.h ui_midi_mapping.h chain-*.frag *.dot *.pb.cc *.pb.h $(OBJS_WITH_MOC:.o=.moc.cpp) ellipsis_label.moc.cpp clickable_label.moc.cpp

PREFIX=/usr/local
install:
	$(INSTALL) -m 755 -o root -g root -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/nageru
	$(INSTALL) -m 755 -o root -g root nageru $(DESTDIR)$(PREFIX)/bin/nageru
	$(INSTALL) -m 644 -o root -g root theme.lua $(DESTDIR)$(PREFIX)/share/nageru/theme.lua
	$(INSTALL) -m 644 -o root -g root simple.lua $(DESTDIR)$(PREFIX)/share/nageru/simple.lua
	$(INSTALL) -m 644 -o root -g root bg.jpeg $(DESTDIR)$(PREFIX)/share/nageru/bg.jpeg
	$(INSTALL) -m 644 -o root -g root akai_midimix.midimapping $(DESTDIR)$(PREFIX)/share/nageru/akai_midimix.midimapping
