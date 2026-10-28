// What this PC can run locally, and which Ollama models to suggest for it.
#pragma once

#include "json.hpp"

// {cpu, ramGb, gpus:[{name, vramGb, integrated}], bestVramGb}
Json detectHardware();

// Ordered suggestions for a read-only code reviewer on this hardware:
// [{model, label, sizeGb, why, fit: "gpu"|"cpu"|"tight", recommended}]
Json recommendModels(const Json& hw);
