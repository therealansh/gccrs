struct Foo {
    one: i32,
    two: i32,
}

fn main() {
    let struct_test = Foo { one: 1, two: 2 };
    let a = struct_test.one;
    let b = struct_test.two;
}
