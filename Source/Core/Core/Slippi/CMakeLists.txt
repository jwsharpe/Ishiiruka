
# Protobuf dependency. Use global or local one?
message("I GOT HERE BUT THIS STILL DOESNT WORK WHY")
if(USE_SHARED_PROTOBUF)
	set(Protobuf_DEBUG TRUE)
	find_package(Protobuf REQUIRED)
	if(Protobuf_VERSION VERSION_LESS 3.0.0)
		message("Installed version of protobuf is too old. Use at least 3.0.0")
	endif()
	set(Protobuf_USE_STATIC_LIBS ON)
	protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS slippicomm.proto)
endif()

set(SRCS slippicomm.pb.cc)
