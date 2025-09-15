#include "choc/containers/choc_Value.h"
#include "choc/platform/choc_Assert.h"
#include "choc/text/choc_StringUtilities.h"
#include "popl.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fmod.hpp>
#include <fmod_errors.h>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <stringzilla/stringzilla.hpp>
#include <unordered_map>
#include <vector>

namespace sz = ashvardanian::stringzilla;
namespace rng = std::ranges;
namespace vw = std::views;
using namespace popl;

enum class AppError {
  FmodSystemCreation,
  FmodSystemInit,
  ArgumentParsing,
  InvalidDsp,
  PluginLoad,
  DspCreation,
  ParameterInfo
};

template <typename T> using Result = std::expected<T, AppError>;

class FmodSystem {
  std::unique_ptr<FMOD::System, void (*)(FMOD::System *)> system_;

public:
  FmodSystem()
      : system_(nullptr, [](FMOD::System *s) {
          if (s)
            s->release();
        }) {
    FMOD::System *sys;
    if (const auto res = FMOD::System_Create(&sys); res == FMOD_OK) {
      system_.reset(sys);
    }
  }

  Result<void> initialize() {
    if (!system_)
      return std::unexpected(AppError::FmodSystemCreation);

    constexpr auto flags = FMOD_INIT_CLIP_OUTPUT | FMOD_INIT_CHANNEL_LOWPASS |
                           FMOD_INIT_CHANNEL_DISTANCEFILTER |
                           FMOD_INIT_VOL0_BECOMES_VIRTUAL;

    const auto res = system_->init(4095, flags, nullptr);
    return res == FMOD_OK ? Result<void>{}
                          : std::unexpected(AppError::FmodSystemInit);
  }

  FMOD::System *get() const noexcept { return system_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(system_); }
};

class FmodDsp {
  std::unique_ptr<FMOD::DSP, void (*)(FMOD::DSP *)> dsp_;

public:
  FmodDsp()
      : dsp_(nullptr, [](FMOD::DSP *d) {
          if (d)
            d->release();
        }) {}
  explicit FmodDsp(FMOD::DSP *dsp)
      : dsp_(dsp, [](FMOD::DSP *d) {
          if (d)
            d->release();
        }) {}

  FMOD::DSP *get() const noexcept { return dsp_.get(); }
  FMOD::DSP *release() noexcept { return dsp_.release(); }
  explicit operator bool() const noexcept { return static_cast<bool>(dsp_); }
};

auto normalize_string(std::string_view input) -> std::string {
  auto result = choc::text::trim(std::string{input});
  return choc::text::toLowerCase(std::move(result));
}

template <typename T> struct FuzzyMatch {
  std::string key;
  T value;
  double confidence;
  std::size_t edit_distance;
};

template <typename T>
auto find_fuzzy_match(const std::unordered_map<std::string, T> &dict,
                      std::string_view query, double cutoff = 0.72)
    -> std::optional<FuzzyMatch<T>> {
  const auto normalized_query = normalize_string(query);
  auto normalized_map =
      dict | vw::transform([](const auto &pair) {
        return std::make_pair(normalize_string(pair.first), pair.first);
      }) |
      rng::to<std::unordered_map<std::string, std::string>>();
  if (auto it = normalized_map.find(normalized_query);
      it != normalized_map.end()) {
    const auto &canonical = it->second;
    return FuzzyMatch<T>{canonical, dict.at(canonical), 1.0, 0};
  }
  const auto best_match =
      rng::min_element(normalized_map, [&](const auto &lhs, const auto &rhs) {
        return choc::text::getLevenshteinDistance(normalized_query, lhs.first) <
               choc::text::getLevenshteinDistance(normalized_query, rhs.first);
      });
  if (best_match == normalized_map.end())
    return std::nullopt;
  const auto distance =
      choc::text::getLevenshteinDistance(normalized_query, best_match->first);
  const auto max_length =
      std::max(normalized_query.size(), best_match->first.size());
  const auto confidence =
      max_length ? 1.0 - static_cast<double>(distance) / max_length : 0.0;
  if (confidence < cutoff)
    return std::nullopt;
  const auto &canonical = best_match->second;
  return FuzzyMatch<T>{canonical, dict.at(canonical), confidence, distance};
}

auto parameter_type_to_string(FMOD_DSP_PARAMETER_TYPE type)
    -> std::string_view {
  switch (type) {
  case FMOD_DSP_PARAMETER_TYPE_FLOAT:
    return "float";
  case FMOD_DSP_PARAMETER_TYPE_INT:
    return "int";
  case FMOD_DSP_PARAMETER_TYPE_BOOL:
    return "bool";
  case FMOD_DSP_PARAMETER_TYPE_DATA:
    return "data";
  default:
    return "unknown";
  }
}

auto float_mapping_to_string(FMOD_DSP_PARAMETER_FLOAT_MAPPING_TYPE type)
    -> std::string_view {
  switch (type) {
  case FMOD_DSP_PARAMETER_FLOAT_MAPPING_TYPE_LINEAR:
    return "linear";
  case FMOD_DSP_PARAMETER_FLOAT_MAPPING_TYPE_AUTO:
    return "auto";
  case FMOD_DSP_PARAMETER_FLOAT_MAPPING_TYPE_PIECEWISE_LINEAR:
    return "piecewise linear";
  default:
    return "unknown";
  }
}

auto data_type_to_string(FMOD_DSP_PARAMETER_DATA_TYPE type)
    -> std::string_view {
  using enum FMOD_DSP_PARAMETER_DATA_TYPE;
  switch (type) {
  case FMOD_DSP_PARAMETER_DATA_TYPE_USER:
    return "user-defined/default";
  case FMOD_DSP_PARAMETER_DATA_TYPE_OVERALLGAIN:
    return "overall gain";
  case FMOD_DSP_PARAMETER_DATA_TYPE_3DATTRIBUTES:
    return "3D Attributes";
  case FMOD_DSP_PARAMETER_DATA_TYPE_SIDECHAIN:
    return "sidechain";
  case FMOD_DSP_PARAMETER_DATA_TYPE_FFT:
    return "FFT";
  case FMOD_DSP_PARAMETER_DATA_TYPE_3DATTRIBUTES_MULTI:
    return "3D attributes (multiple listeners)";
  case FMOD_DSP_PARAMETER_DATA_TYPE_ATTENUATION_RANGE:
    return "attenuation range";
  case FMOD_DSP_PARAMETER_DATA_TYPE_DYNAMIC_RESPONSE:
    return "dynamic response";
  default:
    return "unknown";
  }
}

auto plugin_type_to_string(FMOD_PLUGINTYPE type) -> std::string_view {
  switch (type) {
  case FMOD_PLUGINTYPE_OUTPUT:
    return "output";
  case FMOD_PLUGINTYPE_CODEC:
    return "codec";
  case FMOD_PLUGINTYPE_DSP:
    return "DSP";
  default:
    return "unknown";
  }
}

void print_float_parameter(const FMOD_DSP_PARAMETER_DESC_FLOAT &desc) {
  std::println("  Range: [{}, {}], Default: {}", desc.min, desc.max,
               desc.defaultval);
  std::println("  Mapping: {}", float_mapping_to_string(desc.mapping.type));
  if (desc.mapping.type ==
      FMOD_DSP_PARAMETER_FLOAT_MAPPING_TYPE_PIECEWISE_LINEAR) {
    const auto &pw = desc.mapping.piecewiselinearmapping;
    if (pw.numpoints <= 0) {
      std::println("  (piecewise mapping with zero points)");
      return;
    }
    if (!pw.pointparamvalues || !pw.pointpositions) {
      std::println("  (piecewise mapping points not provided by plugin)");
      return;
    }
    std::println("  Piecewise linear points:");
    for (int i = 0; i < pw.numpoints; ++i) {
      float v = pw.pointparamvalues[i];
      float p = pw.pointpositions[i];
      std::println("    {}: value={}, position={}", i, v, p);
    }
  }
}

void print_int_parameter(const FMOD_DSP_PARAMETER_DESC_INT &desc) {
  std::println("  Range: [{}, {}], Default: {}", desc.min, desc.max,
               desc.defaultval);
  std::println("  Goes to infinity: {}", desc.goestoinf ? "yes" : "no");

  if (desc.valuenames) {
    const auto count = desc.max - desc.min + 1;
    const auto names =
        std::span{desc.valuenames, static_cast<std::size_t>(count)};
    auto joined_names = names |
                        vw::transform([](auto v) { return std::string{v}; }) |
                        vw::join_with(std::string_view(", "));
    std::println("  Values: {}", std::ranges::to<std::string>(joined_names));
  }
}

void print_bool_parameter(const FMOD_DSP_PARAMETER_DESC_BOOL &desc) {
  std::println("  Default: {}", static_cast<bool>(desc.defaultval));

  if (desc.valuenames) {
    std::println("  Values: {}, {}", desc.valuenames[0], desc.valuenames[1]);
  }
}

void print_data_parameter(const FMOD_DSP_PARAMETER_DESC_DATA &desc) {
  std::println("  Data type: {}",
               data_type_to_string(
                   static_cast<FMOD_DSP_PARAMETER_DATA_TYPE>(desc.datatype)));
}

void print_parameter_info(std::int32_t index,
                          const FMOD_DSP_PARAMETER_DESC &desc) {
  std::println("{}:", index);
  std::println("  Name: {}", desc.name ? desc.name : "null");
  std::println("  Label: {}", desc.label ? desc.label : "null");
  std::println("  Description: {}",
               desc.description ? desc.description : "null");
  std::println("  Type: {}", parameter_type_to_string(desc.type));

  switch (desc.type) {
  case FMOD_DSP_PARAMETER_TYPE_FLOAT:
    print_float_parameter(desc.floatdesc);
    break;
  case FMOD_DSP_PARAMETER_TYPE_INT:
    print_int_parameter(desc.intdesc);
    break;
  case FMOD_DSP_PARAMETER_TYPE_BOOL:
    print_bool_parameter(desc.booldesc);
    break;
  case FMOD_DSP_PARAMETER_TYPE_DATA:
    print_data_parameter(desc.datadesc);
    break;
  }
}

void print_dsp_info(FMOD::DSP *dsp) {
  CHOC_ASSERT(dsp != nullptr);
  sz::string name;
  name.resize(32);
  std::uint32_t version;
  std::int32_t channels, config_width, config_height;
  if (const auto res = dsp->getInfo(name.data(), &version, &channels,
                                    &config_width, &config_height);
      res != FMOD_OK) {
    std::println("Can't get DSP info: {}", FMOD_ErrorString(res));
    return;
  }
  name.strip(sz::char_set(sz::ascii_controls()));
  name.strip(sz::char_set(sz::whitespaces()));
  name.resize(std::strlen(name.c_str()));
  std::println("Found DSP: {}", std::string_view(name));
  std::println("Channels: {}, Dialog size: {}x{}", channels, config_width,
               config_height);

  std::int32_t param_count;
  if (const auto res = dsp->getNumParameters(&param_count); res != FMOD_OK) {
    std::println("Couldn't get parameter count: {}", FMOD_ErrorString(res));
    return;
  }

  if (param_count == 0) {
    std::println("DSP has no parameters\n");
    return;
  }

  std::println("Parameters ({}):", param_count);
  for (const auto i : vw::iota(0, param_count)) {
    FMOD_DSP_PARAMETER_DESC *desc;
    if (const auto res = dsp->getParameterInfo(i, &desc); res != FMOD_OK) {
      std::println("Skipping parameter {} ({})", i, FMOD_ErrorString(res));
      continue;
    }
    print_parameter_info(i, *desc);
  }
  std::println();
}

constexpr auto builtin_dsps =
    std::to_array<std::pair<std::string_view, FMOD_DSP_TYPE>>(
        {{"mixer", FMOD_DSP_TYPE_MIXER},
         {"oscillator", FMOD_DSP_TYPE_OSCILLATOR},
         {"lowpass", FMOD_DSP_TYPE_LOWPASS},
         {"itlowpass", FMOD_DSP_TYPE_ITLOWPASS},
         {"highpass", FMOD_DSP_TYPE_HIGHPASS},
         {"echo", FMOD_DSP_TYPE_ECHO},
         {"fader", FMOD_DSP_TYPE_FADER},
         {"flange", FMOD_DSP_TYPE_FLANGE},
         {"distortion", FMOD_DSP_TYPE_DISTORTION},
         {"normalize", FMOD_DSP_TYPE_NORMALIZE},
         {"limiter", FMOD_DSP_TYPE_LIMITER},
         {"parameq", FMOD_DSP_TYPE_PARAMEQ},
         {"pitchshift", FMOD_DSP_TYPE_PITCHSHIFT},
         {"chorus", FMOD_DSP_TYPE_CHORUS},
         {"itecho", FMOD_DSP_TYPE_ITECHO},
         {"compressor", FMOD_DSP_TYPE_COMPRESSOR},
         {"sfxreverb", FMOD_DSP_TYPE_SFXREVERB},
         {"lowpasssimple", FMOD_DSP_TYPE_LOWPASS_SIMPLE},
         {"delay", FMOD_DSP_TYPE_DELAY},
         {"tremolo", FMOD_DSP_TYPE_TREMOLO},
         {"send", FMOD_DSP_TYPE_SEND},
         {"return", FMOD_DSP_TYPE_RETURN},
         {"highpasssimple", FMOD_DSP_TYPE_HIGHPASS_SIMPLE},
         {"pan", FMOD_DSP_TYPE_PAN},
         {"threeeq", FMOD_DSP_TYPE_THREE_EQ},
         {"fft", FMOD_DSP_TYPE_FFT},
         {"loudnessmeter", FMOD_DSP_TYPE_LOUDNESS_METER},
         {"convolutionreverb", FMOD_DSP_TYPE_CONVOLUTIONREVERB},
         {"channelmix", FMOD_DSP_TYPE_CHANNELMIX},
         {"transceiver", FMOD_DSP_TYPE_TRANSCEIVER},
         {"objectpan", FMOD_DSP_TYPE_OBJECTPAN},
         {"multibandeq", FMOD_DSP_TYPE_MULTIBAND_EQ},
         {"multibanddynamics", FMOD_DSP_TYPE_MULTIBAND_DYNAMICS}});

auto process_builtin_dsps(FMOD::System *system,
                          std::span<const std::string> args) -> Result<void> {
  const auto dsp_map = std::unordered_map<std::string, FMOD_DSP_TYPE>{
      builtin_dsps.begin(), builtin_dsps.end()};

  const auto invalid_dsps =
      args | vw::filter([&](const auto &arg) {
        return !find_fuzzy_match(dsp_map, arg).has_value();
      }) |
      rng::to<std::vector<std::string>>();

  if (!invalid_dsps.empty()) {
    for (const auto &invalid : invalid_dsps) {
      std::println("Error: DSP '{}' not found", invalid);
    }

    const auto available_names =
        dsp_map | vw::keys | rng::to<std::vector<std::string>>();
    std::println("Available built-in DSPs: {}",
                 choc::text::joinStrings(available_names, ", "));
    return std::unexpected(AppError::InvalidDsp);
  }
  for (const auto &arg : args) {
    if (const auto match = find_fuzzy_match(dsp_map, arg)) {
      FMOD::DSP *raw_dsp;
      if (const auto res = system->createDSPByType(match->value, &raw_dsp);
          res != FMOD_OK) {
        std::println("Warning: Could not create '{}' DSP: {}", match->key,
                     FMOD_ErrorString(res));
        continue;
      }

      const auto dsp = FmodDsp{raw_dsp};
      print_dsp_info(dsp.get());
    }
  }

  return {};
}

auto load_and_process_plugin(FMOD::System *system,
                             const std::string &plugin_path) -> Result<void> {
  std::uint32_t handle = 0;
  if (const auto res = system->loadPlugin(plugin_path.c_str(), &handle);
      res != FMOD_OK) {
    std::println("Could not load plugin '{}': {}", plugin_path,
                 FMOD_ErrorString(res));
    return std::unexpected(AppError::PluginLoad);
  }

  // Get main plugin info
  sz::string plugin_name;
  plugin_name.resize(4096);
  FMOD_PLUGINTYPE plugin_type;
  std::uint32_t plugin_version;

  if (const auto res = system->getPluginInfo(
          handle, &plugin_type, plugin_name.data(), 4096, &plugin_version);
      res != FMOD_OK) {
    std::println("Can't get plugin info for '{}': {}", plugin_path,
                 FMOD_ErrorString(res));
    return std::unexpected(AppError::ParameterInfo);
  }
  plugin_name.strip(sz::char_set(sz::ascii_controls()));
  plugin_name.strip(sz::char_set(sz::whitespaces()));
  plugin_name.resize(std::strlen(plugin_name.c_str()));
  std::println("Found plugin '{}' v.{}, type: {}", plugin_name.c_str(),
               plugin_version, plugin_type_to_string(plugin_type));

  if (plugin_type == FMOD_PLUGINTYPE_DSP) {
    FMOD::DSP *raw_dsp;
    if (const auto res = system->createDSPByPlugin(handle, &raw_dsp);
        res != FMOD_OK) {
      std::println("Can't create DSP from plugin: {}", FMOD_ErrorString(res));
      return std::unexpected(AppError::DspCreation);
    }

    const auto dsp = FmodDsp{raw_dsp};
    print_dsp_info(dsp.get());
  }

  std::int32_t nested_count;
  if (const auto res = system->getNumNestedPlugins(handle, &nested_count);
      res != FMOD_OK) {
    std::println("Can't get nested plugin count: {}", FMOD_ErrorString(res));
    return std::unexpected(AppError::ParameterInfo);
  }

  if (nested_count > 0) {
    std::println("Plugin has {} nested plugin(s):", nested_count);

    for (const auto i : vw::iota(0, nested_count)) {
      std::uint32_t sub_handle;
      if (const auto res = system->getNestedPlugin(handle, i, &sub_handle);
          res != FMOD_OK) {
        std::println("Warning: skipping nested plugin {}: {}", i,
                     FMOD_ErrorString(res));
        continue;
      }

      sz::string sub_name;
      sub_name.resize(4096);
      FMOD_PLUGINTYPE sub_type;
      std::uint32_t sub_version;

      if (const auto res = system->getPluginInfo(
              sub_handle, &sub_type, sub_name.data(), 4096, &sub_version);
          res != FMOD_OK) {
        std::println("Warning: can't get info for nested plugin {}: {}", i,
                     FMOD_ErrorString(res));
        continue;
      }
      sub_name.strip(sz::char_set(sz::ascii_controls()));
      sub_name.strip(sz::char_set(sz::whitespaces()));
      sub_name.resize(std::strlen(sub_name.c_str()));
      std::println("  Nested plugin '{}' v.{}, type: {}", sub_name.c_str(),
                   sub_version, plugin_type_to_string(sub_type));

      if (sub_type == FMOD_PLUGINTYPE_DSP) {
        FMOD::DSP *raw_sub_dsp;
        if (const auto res =
                system->createDSPByPlugin(sub_handle, &raw_sub_dsp);
            res != FMOD_OK) {
          std::println("Warning: can't create DSP from nested plugin: {}",
                       FMOD_ErrorString(res));
          continue;
        }

        const auto sub_dsp = FmodDsp{raw_sub_dsp};
        print_dsp_info(sub_dsp.get());
      }
    }
  }

  return {};
}

auto process_plugins(FMOD::System *system,
                     std::span<const std::string> plugin_paths)
    -> Result<void> {
  for (const auto &path : plugin_paths) {
    if (const auto result = load_and_process_plugin(system, path); !result) {
      return result;
    }
  }
  return {};
}

int main(int argc, char **argv) {
  auto system = FmodSystem{};
  if (const auto init_result = system.initialize(); !init_result) {
    std::println("Failed to initialize FMOD system");
    return 1;
  }

  auto parser = OptionParser{"Audio DSP Information Tool"};
  auto help_option = parser.add<Switch>("h", "help", "Show help message");
  auto is_plugin_option = parser.add<Implicit<bool>>(
      "p", "plugin", "Process plugin files instead of built-in DSP types",
      false);

  try {
    parser.parse(argc, argv);
  } catch (const std::exception &ex) {
    std::println("Error parsing arguments: {}", ex.what());
    return 1;
  }

  if (help_option->count() > 0) {
    std::cout << parser << '\n';
    std::println("\nExamples:");
    std::println("  {} lowpass highpass echo        # Show built-in DSP info",
                 argv[0]);
    std::println("  {} -p plugin1.dll plugin2.dll  # Show plugin DSP info",
                 argv[0]);
    return 0;
  }

  const auto args = parser.non_option_args();
  if (args.empty()) {
    std::println("No DSP types or plugins specified. Use -h for help.");
    return 1;
  }

  const auto process_result = is_plugin_option->is_set()
                                  ? process_plugins(system.get(), args)
                                  : process_builtin_dsps(system.get(), args);

  if (!process_result) {
    return 1;
  }

  return 0;
}