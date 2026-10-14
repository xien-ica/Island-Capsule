#pragma once
#include "common.hpp"

namespace island {

class CapsuleUI {
public:
  FrameworkElement Create();
  void UpdateMedia(const MediaInfo& info);
  void SetAccent(Color color);
  void SetPlaying(bool playing);
  void SetMaxWidth(double max);

private:
  Border pill_{ nullptr };
  Image art_{ nullptr };
  TextBlock title_{ nullptr };
  TextBlock artist_{ nullptr };
};

} // namespace island
