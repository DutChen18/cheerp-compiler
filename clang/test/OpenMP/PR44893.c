// RUN: %clang -target x86_64-unknown-gnu -fopenmp -O -g -x c %s -S -disable-output -o %t

// Do not crash ;)

void foo(void)
{
#pragma omp critical
  ;
}

void bar(void)
{
  foo();
  foo();
}
