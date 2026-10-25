#include "voice/AudioCoordinator.h"
namespace handheld::voice {
AudioCoordinator& AudioCoordinator::instance() { static AudioCoordinator value; return value; }
}
