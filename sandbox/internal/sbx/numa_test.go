package sbx

import (
	"reflect"
	"testing"
)

func TestContainsInt(t *testing.T) {
	tests := []struct {
		v    []int
		x    int
		want bool
	}{
		{[]int{1, 2, 3}, 2, true},
		{[]int{1, 2, 3}, 4, false},
		{[]int{}, 1, false},
		{nil, 1, false},
		{[]int{5}, 5, true},
		{[]int{0}, 0, true},
	}
	for _, tt := range tests {
		got := containsInt(tt.v, tt.x)
		if got != tt.want {
			t.Errorf("containsInt(%v, %d) = %v, want %v", tt.v, tt.x, got, tt.want)
		}
	}
}

func TestParseCpuList(t *testing.T) {
	tests := []struct {
		input string
		want  []int
	}{
		{"0-3", []int{0, 1, 2, 3}},
		{"0", []int{0}},
		{"0,2,4", []int{0, 2, 4}},
		{"1-3,7", []int{1, 2, 3, 7}},
		{"", nil},
	}
	for _, tt := range tests {
		got := parseCpuList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("parseCpuList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}

func TestCompressCpus(t *testing.T) {
	tests := []struct {
		input []int
		want  string
	}{
		{[]int{0, 1, 2, 3}, "0-3"},
		{[]int{5}, "5"},
		{[]int{0, 2, 4}, "0,2,4"},
		{[]int{0, 1, 2, 3, 8, 9, 10, 11}, "0-3,8-11"},
		{[]int{0, 1, 3, 4, 5, 9}, "0-1,3-5,9"},
		{nil, ""},
		{[]int{}, ""},
	}
	for _, tt := range tests {
		got := compressCpus(tt.input)
		if got != tt.want {
			t.Errorf("compressCpus(%v) = %q, want %q", tt.input, got, tt.want)
		}
	}
}

func TestJoinInts(t *testing.T) {
	tests := []struct {
		input []int
		want  string
	}{
		{[]int{0, 1, 2}, "0,1,2"},
		{[]int{5}, "5"},
		{nil, ""},
	}
	for _, tt := range tests {
		got := joinInts(tt.input)
		if got != tt.want {
			t.Errorf("joinInts(%v) = %q, want %q", tt.input, got, tt.want)
		}
	}
}
