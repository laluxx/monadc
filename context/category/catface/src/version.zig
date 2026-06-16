pub const version = "0.7.6";
pub const codename = "Function Cache";

pub fn banner() []const u8 {
    return "Catface 0.7.6 Function Cache";
}

test "version exists" {
    try @import("std").testing.expect(version.len > 0);
}
