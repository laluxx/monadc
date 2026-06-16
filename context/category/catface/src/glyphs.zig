const model = @import("model.zig");

pub fn kind(k: model.ObjectKind) []const u8 {
    return switch (k) {
        .file => "◇",
        .heading => "◆",
        .record => "▣",
        .script => "⌁",
        .report => "▤",
        .concept => "◯",
        .test_kind => "T",
        .source => "S",
        .function_kind => "λ",
        .info => "I",
        .todo => "!",
        .done => "✓",
        .unknown => "·",
    };
}

pub fn edge(k: model.EdgeKind) []const u8 {
    return switch (k) {
        .contains => "⊃",
        .file_link => "↦",
        .id_link => "⇢",
        .supports => "⊢",
        .supersedes => "≻",
        .verifies => "✓",
        .blocks => "⊣",
        .refines => "≤",
        .classifies_as => "∈",
        .forgets_to => "ƒ",
        .generated_by => "↺",
        .mentions => "∋",
        .unknown => "→",
    };
}

pub const logo = "󰄛 Catface";
pub const query_prompt = "❯ ";
pub const focus = "*";
pub const inactive = "o";
pub const compose = "∘";
pub const identity = "id";
pub const hom = "Hom";
pub const projection = "proj";
