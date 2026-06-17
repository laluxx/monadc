pub const version = "0.11.6";
pub const codename = "Subtree Org Editor Cockpit";

pub fn label() []const u8 {
    return "Catface 0.11.6 Subtree Org Editor Cockpit";
}

test "version exists" {
    try @import("std").testing.expect(version.len > 0);
}
